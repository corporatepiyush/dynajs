// covers: c:src/dyna-aio-hold.h
// F-net-1 (2026-11-16): App responses held during one dispatch are flushed with one send;
// when the socket buffer takes only part, the rest is queued piece by piece. Each piece used to
// be queued through the normal send path, which first tries send() itself: if the peer had read
// in the meantime that piece went out at once and its completion callback ran before the later
// pieces were queued, so a response the App wrote from that callback (the next pipelined one)
// reached the wire ahead of them. Seen as whole responses out of order in
// tests/http/test_http_app_http_app_coalesce.js ("intact 21", 4/160 runs at 24-way load).
// The remainder is now queued with DYN_AIO_NOSEND, which skips the direct send, so every piece
// is queued before any of them completes. This drives dyn_aio_hold_flush deterministically: the
// socket is full so the flush's own send gets nothing out, and the fake queue behaves like a
// just-drained socket (sends and completes at once) unless DYN_AIO_NOSEND is set.
#include "dyna-aio-hold.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)            \
    do {                         \
        if (!(c)) {              \
            printf("FAIL: ");    \
            printf(__VA_ARGS__); \
            printf("\n");        \
            fails++;             \
        }                        \
    } while (0)

#define NPIECE 6
#define MAXLOG 64

typedef struct {
    dyn_aio_cb cb;
    void* ud;
    int tag;
} pending_t;

static dyn_aio_hold_t hold;
static int wire[MAXLOG], nwire;
static pending_t pend[MAXLOG];
static int npend;
static int drained;

static dyn_aio_hold_t* fake_get(dyn_aio_t* a, int fd, uint32_t life)
{
    (void)a;
    (void)fd;
    (void)life;
    return &hold;
}

static int fake_queue(dyn_aio_t* a, int fd, const void* buf, size_t len, int flags,
    dyn_aio_cb cb, void* udata)
{
    int tag = ((const uint8_t*)buf)[0];
    (void)fd;
    (void)len;
    if (!(flags & DYN_AIO_NOSEND) && drained && npend == 0) {
        wire[nwire++] = tag;
        if (cb)
            cb(a, (int)len, NULL, 0, udata);
        return 0;
    }
    pend[npend].cb = cb;
    pend[npend].ud = udata;
    pend[npend].tag = tag;
    npend++;
    return 0;
}

static void on_piece(dyn_aio_t* a, int res, const uint8_t* b, unsigned n, void* ud)
{
    uint8_t next = 100;
    (void)b;
    (void)n;
    (void)res;
    if ((intptr_t)ud == 0)
        fake_queue(a, -1, &next, 1, 0, NULL, NULL);
}

static void drain_pending(dyn_aio_t* a)
{
    int i;
    for (i = 0; i < npend; i++) {
        wire[nwire++] = pend[i].tag;
        if (pend[i].cb)
            pend[i].cb(a, 1, NULL, 0, pend[i].ud);
    }
    npend = 0;
}

int main(void)
{
    int sv[2], i, fl;
    uint8_t piece[NPIECE][2048];
    static uint8_t fill[1 << 16];
    dyn_aio_t* a = (dyn_aio_t*)&hold;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("FAIL: socketpair\n");
        return 1;
    }
    fl = fcntl(sv[0], F_GETFL);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    while (send(sv[0], fill, sizeof fill, 0) > 0)
        ;
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK, "the send buffer is full before the flush");

    memset(&hold, 0, sizeof hold);
    for (i = 0; i < NPIECE; i++) {
        memset(piece[i], i, sizeof piece[i]);
        CHECK(dyn_aio_hold_append(&hold, piece[i], sizeof piece[i], on_piece, (void*)(intptr_t)i) == 0,
            "append piece %d", i);
    }
    drained = 1;
    dyn_aio_hold_flush(a, sv[0], 0, 0, fake_get, fake_queue);
    CHECK(nwire == 0, "no piece reaches the wire before every piece is queued (got %d early)", nwire);
    CHECK(npend == NPIECE, "all %d pieces are queued (got %d)", NPIECE, npend);
    drain_pending(a);
    drain_pending(a);
    CHECK(nwire == NPIECE + 1, "every piece and the follow-up reach the wire (got %d)", nwire);
    for (i = 0; i < NPIECE && i < nwire; i++)
        CHECK(wire[i] == i, "piece %d leaves in position %d (got %d)", i, i, wire[i]);
    CHECK(nwire == NPIECE + 1 && wire[NPIECE] == 100,
        "a response written from a piece's completion follows the whole remainder");
    dyn_aio_hold_release(&hold);
    close(sv[0]);
    close(sv[1]);
    if (fails) {
        printf("test_aio_hold_flush_order: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_aio_hold_flush_order: ok\n");
    return 0;
}
