#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define FILE_LEN (512u * 1024u)

static void fill(unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        b[i] = (unsigned char)(i * 31u + 7u);
}

static int make_temp_file(char *path, size_t len)
{
    unsigned char block[64 * 1024];
    int fd = mkstemp(path);
    size_t w = 0;
    if (fd < 0)
        return -1;
    fill(block, sizeof(block));
    while (w < len) {
        size_t chunk = len - w > sizeof(block) ? sizeof(block) : len - w;
        if (write(fd, block, chunk) < 0) { close(fd); return -1; }
        w += chunk;
    }
    return fd;
}

static int shrunken_pair(int sv[2])
{
    int sndbuf = 16 * 1024;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
        return -1;
    dyn_net_set_nonblock(sv[0]);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    return 0;
}

static void pump_until(dyn_aio_t *a, int peer, unsigned char *rx, size_t cap,
                       size_t *got, const volatile int *done, int max_ms)
{
    int spins;
    for (spins = 0; !*done && spins < max_ms / 5; spins++) {
        ssize_t n;
        while (*got < cap && (n = read(peer, rx + *got, cap - *got)) > 0)
            *got += (size_t)n;
        if (*got >= cap)
            return;
        dyn_aio_run(a, 5);
    }
    for (spins = 0; *got < cap && spins < 200; spins++) {
        ssize_t n;
        while (*got < cap && (n = read(peer, rx + *got, cap - *got)) > 0)
            *got += (size_t)n;
        usleep(1000);
    }
}

static int cb1_calls, cb1_res;
static void on_first(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; cb1_calls++; cb1_res = res; }

static int cb2_calls;
static void on_second(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                      void *ud)
{ (void)a; (void)res; (void)buf; (void)n; (void)ud; cb2_calls++; }

static int cbA, cbB, cbC, cbH;
static void cnt_a(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *ud)
{ (void)a; (void)b; (void)n; (void)ud; (void)res; cbA++; }
static void cnt_b(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *ud)
{ (void)a; (void)b; (void)n; (void)ud; (void)res; cbB++; }
static void cnt_c(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *ud)
{ (void)a; (void)b; (void)n; (void)ud; (void)res; cbC++; }
static void cnt_h(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *ud)
{ (void)a; (void)b; (void)n; (void)ud; (void)res; cbH++; }

static int reset_calls, reset_res;
static void on_reset(dyn_aio_t *a, int res, const uint8_t *b, unsigned n, void *ud)
{ (void)a; (void)b; (void)n; (void)ud; reset_calls++; reset_res = res; }

int main(void)
{
    char path[] = "/tmp/dyn_uring_sf_XXXXXX";
    unsigned char *rx, *want;
    dyn_aio_t *a;
    int sv[2], ffd, ffd2, spins;

    setvbuf(stdout, NULL, _IOLBF, 0);

    ffd = make_temp_file(path, FILE_LEN);
    CHECK(ffd >= 0, "temp file");
    if (ffd < 0) return 1;
    close(ffd);
    ffd = -1;
    rx = (unsigned char *)malloc(FILE_LEN + 4096);
    want = (unsigned char *)malloc(FILE_LEN);
    CHECK(rx && want, "alloc rx/want");
    if (!rx || !want) return 1;
    {
        size_t off = 0;
        while (off < FILE_LEN) {
            size_t chunk = FILE_LEN - off > 65536 ? 65536 : FILE_LEN - off;
            fill(want + off, chunk);
            off += chunk;
        }
    }

    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a) return 1;

    {
        size_t got = 0;
        cb1_calls = cb1_res = 0;
        ffd = open(path, O_RDONLY);
        CHECK(ffd >= 0, "open (1)");
        CHECK(shrunken_pair(sv) == 0, "socketpair (1)");
        CHECK(dyn_aio_sendfile(a, sv[0], ffd, 0, FILE_LEN, on_first, NULL) == 0,
              "sendfile accepted (returns 0)");
        CHECK(dyn_aio_queued(a, sv[0]) > 0,
              "the transfer must owe bytes while it streams (queued=%zu)",
              dyn_aio_queued(a, sv[0]));
        pump_until(a, sv[1], rx, FILE_LEN, &got, &cb1_calls, 5000);
        CHECK(cb1_calls == 1, "completion fired %d times, want exactly 1",
              cb1_calls);
        CHECK(cb1_res == (int)FILE_LEN,
              "completion res=%d, want %d (this backend reports the byte "
              "count; the readiness backend reports 0-ok)", cb1_res,
              (int)FILE_LEN);
        CHECK(got == FILE_LEN, "peer received %zu bytes, want %u", got,
              FILE_LEN);
        CHECK(memcmp(rx, want, FILE_LEN) == 0,
              "peer bytes are not the file's pattern -- a count-only oracle "
              "would pass an offset or skipped-region error");
        CHECK(dyn_aio_queued(a, sv[0]) == 0,
              "the fd must owe nothing after completion (queued=%zu)",
              dyn_aio_queued(a, sv[0]));
        dyn_aio_close(a, sv[0]);
        close(sv[1]);
        printf("  ok  1: %u KiB streamed byte-exact via splice\n",
               FILE_LEN / 1024);
    }

    {
        size_t got = 0;
        const size_t WOFF = 1000, WLEN = 1000;
        cb1_calls = cb1_res = 0;
        ffd = open(path, O_RDONLY);
        CHECK(ffd >= 0, "open (2)");
        CHECK(shrunken_pair(sv) == 0, "socketpair (2)");
        CHECK(dyn_aio_sendfile(a, sv[0], ffd, (off_t)WOFF, WLEN, on_first,
                               NULL) == 0, "offset sendfile accepted");
        pump_until(a, sv[1], rx, WLEN, &got, &cb1_calls, 2000);
        CHECK(cb1_calls == 1 && cb1_res == (int)WLEN,
              "window completion: calls=%d res=%d want 1/%d", cb1_calls,
              cb1_res, (int)WLEN);
        CHECK(got == WLEN, "window: peer got %zu bytes, want %zu", got, WLEN);
        CHECK(memcmp(rx, want + WOFF, WLEN) == 0,
              "window bytes are not the file's pattern at offset %zu", WOFF);
        dyn_aio_close(a, sv[0]);
        close(sv[1]);
        printf("  ok  2: offset window [%zu,%zu) is byte-exact\n", WOFF,
               WOFF + WLEN);
    }

    {
        size_t got = 0;
        int r;
        cb1_calls = cb1_res = cb2_calls = 0;
        CHECK(shrunken_pair(sv) == 0, "socketpair (3)");
        ffd = open(path, O_RDONLY);
        CHECK(ffd >= 0, "open (3, first)");
        CHECK(dyn_aio_sendfile(a, sv[0], ffd, 0, FILE_LEN, on_first, NULL) == 0,
              "first sendfile accepted");
        CHECK(dyn_aio_queued(a, sv[0]) > 0,
              "first transfer in flight (queued=%zu) -- or the busy case is "
              "vacuous", dyn_aio_queued(a, sv[0]));
        ffd2 = open(path, O_RDONLY);
        CHECK(ffd2 >= 0, "open (second)");
        errno = 0;
        r = dyn_aio_sendfile(a, sv[0], ffd2, 0, FILE_LEN, on_second, NULL);
        CHECK(r == -1, "a second sendfile on a busy fd returned %d, want -1",
              r);
        CHECK(errno == EBUSY, "refusal must say EBUSY, not a stale errno (got %d)",
              errno);
        close(ffd2);
        pump_until(a, sv[1], rx, FILE_LEN, &got, &cb1_calls, 5000);
        CHECK(cb1_calls == 1, "the first transfer completed %d times, want 1",
              cb1_calls);
        CHECK(cb1_res == (int)FILE_LEN, "first res=%d, want %d", cb1_res,
              (int)FILE_LEN);
        CHECK(cb2_calls == 0,
              "the REFUSED transfer fired %d callbacks; refusing means no "
              "completion exists to fire", cb2_calls);
        CHECK(got == FILE_LEN && memcmp(rx, want, FILE_LEN) == 0,
              "peer bytes wrong after the refused transfer");
        dyn_aio_close(a, sv[0]);
        close(sv[1]);
        printf("  ok  3: second sendfile in flight -> -1/EBUSY, first intact\n");
    }

    {
        enum { LA = 96u * 1024u, LB = 48u * 1024u, LC = 24u * 1024u };
        unsigned char *pa = (unsigned char *)malloc(LA);
        unsigned char *pb = (unsigned char *)malloc(LB);
        unsigned char *pc = (unsigned char *)malloc(LC);
        size_t got = 0;
        cbA = cbB = cbC = 0;
        CHECK(pa && pb && pc, "alloc ABC");
        if (!pa || !pb || !pc) return 1;
        memset(pa, 0xAA, LA);
        memset(pb, 0xBB, LB);
        memset(pc, 0xCC, LC);
        CHECK(shrunken_pair(sv) == 0, "socketpair (4)");
        CHECK(dyn_aio_send(a, sv[0], pa, LA, 0, cnt_a, NULL) == 0, "send A");
        CHECK(dyn_aio_send(a, sv[0], pb, LB, 0, cnt_b, NULL) == 0, "send B");
        CHECK(dyn_aio_send(a, sv[0], pc, LC, 0, cnt_c, NULL) == 0, "send C");
        CHECK(dyn_aio_queued(a, sv[0]) > 0, "A/B/C still owe bytes");
        {
            volatile int any = 0;
            pump_until(a, sv[1], rx, LA + LB + LC, &got, &any, 5000);
        }
        CHECK(got == LA + LB + LC, "peer received %zu bytes, want %zu", got,
              (size_t)(LA + LB + LC));
        CHECK(rx[0] == 0xAA && rx[LA - 1] == 0xAA, "prefix is not A");
        CHECK(rx[LA] == 0xBB && rx[LA + LB - 1] == 0xBB, "middle is not B");
        CHECK(rx[LA + LB] == 0xCC && rx[LA + LB + LC - 1] == 0xCC,
              "suffix is not C");
        {
            size_t i, bad = 0;
            for (i = 0; i < LA; i++) if (rx[i] != 0xAA) bad++;
            for (i = LA; i < LA + LB; i++) if (rx[i] != 0xBB) bad++;
            for (i = LA + LB; i < LA + LB + LC; i++) if (rx[i] != 0xCC) bad++;
            CHECK(bad == 0, "%zu bytes out of order across A||B||C", bad);
        }
        for (spins = 0; (cbA + cbB + cbC < 3) && spins < 400; spins++)
            dyn_aio_run(a, 5);
        CHECK(cbA == 1 && cbB == 1 && cbC == 1,
              "A/B/C completions: %d/%d/%d, want 1/1/1", cbA, cbB, cbC);
        CHECK(dyn_aio_queued(a, sv[0]) == 0, "fd owes nothing after A/B/C");
        dyn_aio_close(a, sv[0]);
        close(sv[1]);
        free(pa); free(pb); free(pc);
        printf("  ok  4: A,B,C queued without draining arrive as A||B||C\n");
    }

    {
        enum { LH = 32u * 1024u, LF = 256u * 1024u };
        unsigned char *ph = (unsigned char *)malloc(LH);
        size_t got = 0;
        cbH = cb1_calls = cb1_res = 0;
        CHECK(ph != NULL, "alloc header");
        if (!ph) return 1;
        memset(ph, 0xDD, LH);
        CHECK(shrunken_pair(sv) == 0, "socketpair (5)");
        ffd = open(path, O_RDONLY);
        CHECK(ffd >= 0, "open (5)");
        CHECK(dyn_aio_send(a, sv[0], ph, LH, 0, cnt_h, NULL) == 0,
              "header send");
        CHECK(dyn_aio_sendfile(a, sv[0], ffd, 0, LF, on_first, NULL) == 0,
              "body sendfile accepted behind a draining header");
        {
            volatile int any = 0;
            pump_until(a, sv[1], rx, LH + LF, &got, &any, 5000);
        }
        CHECK(got == LH + LF, "peer received %zu bytes, want %zu", got,
              (size_t)(LH + LF));
        {
            size_t i, bad = 0;
            for (i = 0; i < LH; i++) if (rx[i] != 0xDD) bad++;
            CHECK(bad == 0, "%zu header bytes not first: the body overtook "
                  "the buffered prefix", bad);
            CHECK(memcmp(rx + LH, want, LF) == 0,
                  "body bytes are not the file's pattern after the header");
        }
        for (spins = 0; (cbH + cb1_calls < 2) && spins < 400; spins++)
            dyn_aio_run(a, 5);
        CHECK(cbH == 1 && cb1_calls == 1 && cb1_res == (int)LF,
              "header/body completions: h=%d f=%d res=%d want 1/1/%d", cbH,
              cb1_calls, cb1_res, (int)LF);
        dyn_aio_close(a, sv[0]);
        close(sv[1]);
        free(ph);
        printf("  ok  5: header then sendfile body arrive in order\n");
    }

    {
        enum { LQ = 200u * 1024u };
        unsigned char *pq = (unsigned char *)malloc(LQ);
        reset_calls = reset_res = cb1_calls = 0;
        CHECK(pq != NULL, "alloc Q");
        if (!pq) return 1;
        memset(pq, 0xEE, LQ);
        CHECK(shrunken_pair(sv) == 0, "socketpair (6)");
        CHECK(dyn_aio_send(a, sv[0], pq, LQ, 0, on_reset, NULL) == 0,
              "send Q (will be in flight at close)");
        CHECK(dyn_aio_send(a, sv[0], pq, LQ, 0, on_reset, NULL) == 0,
              "send Q2 (will be queued at close)");
        CHECK(dyn_aio_queued(a, sv[0]) > 0, "Q/Q2 owe bytes before close");
        dyn_aio_close(a, sv[0]);
        for (spins = 0; spins < 100; spins++)
            dyn_aio_run(a, 5);
        CHECK(reset_calls == 2,
              "close must complete both queued+in-flight sends exactly once "
              "(got %d)", reset_calls);
        CHECK(reset_res == -ECONNRESET, "close completion res=%d, want %d",
              reset_res, -ECONNRESET);
        close(sv[1]);
        free(pq);
        printf("  ok  6: close flushes queued writes with -ECONNRESET, no UAF\n");
    }

    for (spins = 0; dyn_aio_inflight(a) > 0 && spins < 100; spins++)
        dyn_aio_run(a, 5);
    CHECK(dyn_aio_inflight(a) == 0, "inflight=%zu after all tests, want 0",
          dyn_aio_inflight(a));
    dyn_aio_free(a);
    free(rx);
    free(want);
    unlink(path);

    if (fails == 0) printf("test_uring_sendfile: all tests passed\n");
    else printf("test_uring_sendfile: %d FAILED\n", fails);
    return fails != 0;
}

#else
int main(void)
{
    printf("test_uring_sendfile: skipped (io_uring backend not built)\n");
    return 0;
}
#endif
