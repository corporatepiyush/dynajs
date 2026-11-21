// covers: c:src/dyna-aio-hold.h
// net-5 (2026-11-21): the per-fd write-coalescing buffer (4 KiB data + callback array) used to stay
// attached to its fd after every flush, so each idle connection that had written once kept ~5 KiB:
// 5318 B/conn at 8192 echo connections against node's 4164 and bun's 1480. A flushed or released
// hold now gives its buffers to a per-thread pool of at most DYN_AIO_HOLD_POOL_N spares (each at
// most DYN_AIO_HOLD_POOL_CAP), and the next hold to append on any fd takes one back, so memory
// follows the number of connections writing at once, not the number ever written.
// Pinned: after a flush and after a release the hold owns nothing; 1000 connections flushing one
// after another reuse one buffer (pointer identity: no malloc per write); the pool never exceeds its
// bound; an oversized buffer is freed, not pooled; bytes and completion order are unchanged; the
// drain frees every spare (LeakSanitizer).
// Mutants (red): flush re-attaching the buffer to the hold (retention); the pool bound removed; the
// oversized check removed; the drain dropped (leak).
#include "dyna-aio-hold.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...)                \
    do {                             \
        checks++;                    \
        if (!(c)) {                  \
            printf("FAIL: ");        \
            printf(__VA_ARGS__);     \
            printf("\n");            \
            fails++;                 \
        }                            \
    } while (0)

#define NCONN 1000

static dyn_aio_hold_t holds[NCONN];
static int cur;
static unsigned done_n, done_bytes;

static dyn_aio_hold_t* get_hold(dyn_aio_t* a, int fd, uint32_t life)
{
    (void)a;
    (void)fd;
    (void)life;
    return &holds[cur];
}

static int queue_never(dyn_aio_t* a, int fd, const void* buf, size_t len, int flags, dyn_aio_cb cb, void* udata)
{
    (void)a;
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)cb;
    (void)udata;
    errno = EIO;
    return -1;
}

static void on_done(dyn_aio_t* a, int res, const uint8_t* data, unsigned len, void* ud)
{
    (void)a;
    (void)data;
    (void)len;
    done_n++;
    if (res > 0)
        done_bytes += (unsigned)res;
    if ((intptr_t)ud != (intptr_t)done_n)
        printf("FAIL: completion %u carried udata %ld\n", done_n, (long)(intptr_t)ud), fails++;
}

static void drain_peer(int fd, unsigned want, unsigned seed)
{
    unsigned char b[8192];
    unsigned got = 0, bad = 0;
    while (got < want) {
        ssize_t n = read(fd, b, sizeof(b));
        unsigned i;
        if (n <= 0)
            break;
        for (i = 0; i < (unsigned)n; i++)
            bad += b[i] != (unsigned char)(seed + got + i);
        got += (unsigned)n;
    }
    CHECK(got == want && bad == 0, "peer read %u of %u bytes, %u wrong", got, want, bad);
}

int main(void)
{
    int sv[2], big = 1 << 20, i;
    unsigned char msg[600];
    uint8_t* first = NULL;
    unsigned distinct = 0, retained = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return 2;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    fcntl(sv[0], F_SETFL, O_NONBLOCK);

    done_n = 0;
    for (i = 0; i < NCONN; i++) {
        unsigned j;
        cur = i;
        for (j = 0; j < 2; j++) {
            unsigned k;
            for (k = 0; k < 300; k++)
                msg[k] = (unsigned char)((unsigned)i * 600u + j * 300u + k);
            dyn_aio_hold_append(&holds[i], msg, 300, on_done, (void*)(intptr_t)(done_n + j + 1));
        }
        if (!first)
            first = holds[i].buf;
        else if (holds[i].buf != first)
            distinct++;
        dyn_aio_hold_flush(NULL, sv[0], 0, 0, get_hold, queue_never);
        retained += holds[i].buf != NULL || holds[i].cbs != NULL || holds[i].cap != 0;
        drain_peer(sv[1], 600, (unsigned)i * 600u);
    }
    CHECK(retained == 0, "%u of %d holds still own a buffer after their flush", retained, NCONN);
    CHECK(distinct == 0, "%u of %d sequential connections got a fresh buffer instead of the pooled one", distinct, NCONN);
    CHECK(done_n == 2 * NCONN && done_bytes == 600u * NCONN, "completions %u bytes %u", done_n, done_bytes);
    CHECK(dyn_aio_hold_pool_n == 1, "pool holds %u spares after sequential flushes (want 1)", dyn_aio_hold_pool_n);

    for (i = 0; i < NCONN; i++)
        dyn_aio_hold_append(&holds[i], msg, 100, NULL, NULL);
    CHECK(dyn_aio_hold_pool_n == 0, "concurrent holds drained the pool (%u left)", dyn_aio_hold_pool_n);
    for (i = 0; i < NCONN; i++)
        dyn_aio_hold_release(&holds[i]);
    CHECK(dyn_aio_hold_pool_n == DYN_AIO_HOLD_POOL_N, "pool bounded at %u after %d releases (got %u)",
        DYN_AIO_HOLD_POOL_N, NCONN, dyn_aio_hold_pool_n);
    for (i = 0; i < NCONN; i++)
        retained += holds[i].buf != NULL || holds[i].cbs != NULL;
    CHECK(retained == 0, "released holds still own buffers (%u)", retained);

    dyn_aio_hold_pool_drain();
    CHECK(dyn_aio_hold_pool_n == 0, "drain empties the pool");
    {
        uint8_t* huge = (uint8_t*)malloc(DYN_AIO_HOLD_POOL_CAP * 2);
        static unsigned char blob[DYN_AIO_HOLD_POOL_CAP + 1];
        dyn_aio_hold_append(&holds[0], blob, sizeof(blob), NULL, NULL);
        CHECK(holds[0].cap > DYN_AIO_HOLD_POOL_CAP, "a big write grows the hold (%zu)", holds[0].cap);
        dyn_aio_hold_release(&holds[0]);
        CHECK(dyn_aio_hold_pool_n == 0, "an oversized buffer is freed, not pooled (%u spares)", dyn_aio_hold_pool_n);
        free(huge);
    }
    dyn_aio_hold_pool_drain();
    close(sv[0]);
    close(sv[1]);
    printf("test_aio_hold_pool: %d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
