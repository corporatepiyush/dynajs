#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && !(defined(CONFIG_IO_URING) && defined(__linux__))

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

static int cb1_calls, cb1_res;
static int cb2_calls;

static void on_first(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; cb1_calls++; cb1_res = res; }

static void on_second(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                      void *ud)
{ (void)a; (void)res; (void)buf; (void)n; (void)ud; cb2_calls++; }

static void fill(unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        b[i] = (unsigned char)(i * 31u + 7u);
}

int main(void)
{
    char path[] = "/tmp/dyn_sf_busy_XXXXXX";
    unsigned char block[64 * 1024], *rx;
    dyn_aio_t *a;
    int sv[2], tmpfd, fd1, fd2, sndbuf = 16 * 1024, r, spins;
    size_t got = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    tmpfd = mkstemp(path);
    CHECK(tmpfd >= 0, "mkstemp");
    if (tmpfd < 0) return 1;
    fill(block, sizeof(block));
    {
        size_t w = 0;
        while (w < FILE_LEN) {
            size_t chunk = FILE_LEN - w > sizeof(block) ? sizeof(block)
                                                        : FILE_LEN - w;
            if (write(tmpfd, block, chunk) < 0) {
                printf("FAIL: temp write\n");
                return 1;
            }
            w += chunk;
        }
    }
    close(tmpfd);

    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a) return 1;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0,
          "SO_SNDBUF");
    dyn_net_set_nonblock(sv[0]);

    fd1 = open(path, O_RDONLY);
    CHECK(fd1 >= 0, "open (first)");
    CHECK(dyn_aio_sendfile(a, sv[0], fd1, 0, FILE_LEN, on_first, NULL) == 0,
          "first dyn_aio_sendfile must be accepted (returns 0)");
    CHECK(dyn_aio_queued(a, sv[0]) > 0,
          "the first transfer must still owe bytes, or the busy case below is "
          "vacuous (queued=%zu)", dyn_aio_queued(a, sv[0]));

    fd2 = open(path, O_RDONLY);
    CHECK(fd2 >= 0, "open (second)");
    errno = 0;
    r = dyn_aio_sendfile(a, sv[0], fd2, 0, FILE_LEN, on_second, NULL);
    CHECK(r == -1, "a second sendfile on a busy fd returned %d, want -1 -- "
          "the overwrite leaks the first fd and strands its callback", r);
    CHECK(errno == EBUSY, "refusal must say EBUSY, not a stale errno (got %d)",
          errno);
    close(fd2);

    rx = (unsigned char *)malloc(FILE_LEN);
    CHECK(rx != NULL, "alloc rx");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    for (spins = 0; cb1_calls == 0 && spins < 600; spins++) {
        ssize_t n;
        while ((n = read(sv[1], rx + got, FILE_LEN - got)) > 0)
            got += (size_t)n;
        dyn_aio_run(a, 10);
    }
    for (spins = 0; got < FILE_LEN && spins < 100; spins++) {
        ssize_t n;
        while ((n = read(sv[1], rx + got, FILE_LEN - got)) > 0)
            got += (size_t)n;
        usleep(1000);
    }

    CHECK(cb1_calls == 1,
          "the first transfer completed %d times, want exactly 1", cb1_calls);
    CHECK(cb1_res == 0,
          "first completion res=%d, want 0 -- this backend reports 0-ok for a "
          "finished file send; the byte count is the peer's to prove", cb1_res);
    CHECK(cb2_calls == 0,
          "the REFUSED transfer fired %d callbacks; refusing means no "
          "completion exists to fire", cb2_calls);
    CHECK(got == FILE_LEN, "peer received %zu bytes, want %u", got, FILE_LEN);
    {
        unsigned char *want = (unsigned char *)malloc(FILE_LEN);
        CHECK(want != NULL, "alloc pattern");
        if (want) {
            size_t off = 0;
            while (off < FILE_LEN) {
                size_t chunk = FILE_LEN - off > sizeof(block) ? sizeof(block)
                                                              : FILE_LEN - off;
                fill(block, chunk);
                memcpy(want + off, block, chunk);
                off += chunk;
            }
            CHECK(memcmp(rx, want, FILE_LEN) == 0,
                  "received bytes are not the file's pattern -- a count-only "
                  "check would pass an offset or skipped-region error");
            free(want);
        }
    }
    CHECK(dyn_aio_queued(a, sv[0]) == 0,
          "the fd must owe nothing after the completion (queued=%zu)",
          dyn_aio_queued(a, sv[0]));

    dyn_aio_close(a, sv[0]);
    close(sv[1]);
    dyn_aio_free(a);
    free(rx);
    unlink(path);

    if (fails == 0) printf("test_aio_sendfile_busy: all tests passed\n");
    else printf("test_aio_sendfile_busy: %d FAILED\n", fails);
    return fails != 0;
}

#else
int main(void)
{
    printf("test_aio_sendfile_busy: skipped (readiness backend not built)\n");
    return 0;
}
#endif
