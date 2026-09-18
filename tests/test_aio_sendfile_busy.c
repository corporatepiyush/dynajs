/* test_aio_sendfile_busy.c -- the sendfile slot is single-depth, and a second
 * transfer on the same socket must be REFUSED while one is in flight.
 *
 * The buffered-send side queues (w_qhead) precisely because "a second send
 * overwrote the first -- losing its bytes, leaking its buffer and never
 * running its callback"; the file side deliberately has NO queue. Before the
 * EBUSY guard, a second dyn_aio_sendfile on the same out_fd while
 * w_file_rem > 0 overwrote w_file_fd/off/cb: the first file fd leaked (the
 * adapter owns it, so the caller could not even close it) and its callback
 * never fired, stranding whatever reference the caller held per transfer.
 *
 * Pins the contract three ways:
 *   - the second call returns -1/EBUSY, not 0 -- the observable refusal
 *   - the FIRST transfer still completes, exactly once, with res == 0
 *     (this backend reports 0-ok; the byte count is proven on the peer)
 *   - the refused transfer never delivers a completion of its own
 *
 * Byte count and content are verified on the peer end of a socketpair: a
 * sendfile that "completes" with the wrong number of bytes on the wire would
 * pass a return-value-only oracle. The sender's SO_SNDBUF is shrunk so the
 * first call provably leaves w_file_rem > 0 (partial EAGAIN) -- otherwise the
 * whole transfer could go inline, nothing would be in flight, and the EBUSY
 * assertion would be vacuous.
 *
 * Build/run standalone (mirrors `make test-aio-disk`):
 *   cc -g -O1 -fsanitize=address,undefined -std=gnu17 -D_GNU_SOURCE \
 *      -DCONFIG_NATIVE_MODULES -Isrc tests/test_aio_sendfile_busy.c \
 *      src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c \
 *      src/cutils.c -lpthread -o /tmp/test_aio_sendfile_busy
 */
#include "dyna-aio.h"

/* dyna-aio.c is the READINESS backend and is compiled out when CONFIG_IO_URING
 * selects the io_uring one on Linux (same guard as its line 20) -- there is no
 * code under test in that build, so skip rather than fail. */
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

#define FILE_LEN (512u * 1024u)    /* >> any socket buffer: partial guaranteed */

/* Separate recorders: the leak the guard prevents is exactly "the FIRST
 * callback never fires", which a shared counter could not distinguish. */
static int cb1_calls, cb1_res;
static int cb2_calls;

static void on_first(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                     void *ud)
{ (void)a; (void)buf; (void)n; (void)ud; cb1_calls++; cb1_res = res; }

static void on_second(dyn_aio_t *a, int res, const uint8_t *buf, unsigned n,
                      void *ud)
{ (void)a; (void)res; (void)buf; (void)n; (void)ud; cb2_calls++; }

/* Position-dependent fill, so a transfer that starts at the wrong offset or
 * skips a region cannot match a constant-pattern coincidence. */
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

    /* Line-buffered: a deadlock in this file is a real failure mode, and fully
     * buffered stdout throws away every result printed before it. */
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
            /* the block IS the pattern repeated: fill(block, 64K)'s prefix
             * equals fill(block, chunk), so any tail chunk matches too */
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
    /* Shrink the send buffer so 512 KiB cannot go inline: the first call must
     * return with bytes still owed, or there is nothing in flight to be busy
     * against. Doubling means the kernel may keep more than asked -- the size
     * is chosen so even doubled it is nowhere near FILE_LEN. */
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0,
          "SO_SNDBUF");
    /* The adapter drives the socket from WRITE readiness and treats EAGAIN as
     * "come back later", which only a non-blocking fd reports. */
    dyn_net_set_nonblock(sv[0]);

    /* ---- first transfer: accepted, and provably still in flight ---- */
    fd1 = open(path, O_RDONLY);
    CHECK(fd1 >= 0, "open (first)");
    CHECK(dyn_aio_sendfile(a, sv[0], fd1, 0, FILE_LEN, on_first, NULL) == 0,
          "first dyn_aio_sendfile must be accepted (returns 0)");
    CHECK(dyn_aio_queued(a, sv[0]) > 0,
          "the first transfer must still owe bytes, or the busy case below is "
          "vacuous (queued=%zu)", dyn_aio_queued(a, sv[0]));

    /* ---- second transfer on the SAME socket: refused, nothing overwritten */
    fd2 = open(path, O_RDONLY);
    CHECK(fd2 >= 0, "open (second)");
    errno = 0;
    r = dyn_aio_sendfile(a, sv[0], fd2, 0, FILE_LEN, on_second, NULL);
    CHECK(r == -1, "a second sendfile on a busy fd returned %d, want -1 -- "
          "the overwrite leaks the first fd and strands its callback", r);
    CHECK(errno == EBUSY, "refusal must say EBUSY, not a stale errno (got %d)",
          errno);
    close(fd2);   /* refused, so ownership never transferred to the adapter */

    /* ---- pump: drain the peer so the first transfer can finish ---- */
    rx = (unsigned char *)malloc(FILE_LEN);
    CHECK(rx != NULL, "alloc rx");
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    /* 600 x 10ms is generous for half a meg over a socketpair; against the
     * unguarded code the first callback never fires, so the budget is also
     * the bound on how long the mutation run takes to fail. */
    for (spins = 0; cb1_calls == 0 && spins < 600; spins++) {
        ssize_t n;
        /* drain BEFORE the step: the socket only reports writable again once
         * the peer has taken the backlog, and the filter is level-triggered */
        while ((n = read(sv[1], rx + got, FILE_LEN - got)) > 0)
            got += (size_t)n;
        dyn_aio_run(a, 10);
    }
    /* the completion means the kernel accepted everything; a last pass takes
     * what is still sitting in the buffer */
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

    dyn_aio_close(a, sv[0]);   /* the adapter closed fd1 in the completion */
    close(sv[1]);
    dyn_aio_free(a);
    free(rx);
    unlink(path);

    if (fails == 0) printf("test_aio_sendfile_busy: all tests passed\n");
    else printf("test_aio_sendfile_busy: %d FAILED\n", fails);
    return fails != 0;
}

#else /* io_uring backend selected: dyna-aio.c compiles to nothing */
int main(void)
{
    printf("test_aio_sendfile_busy: skipped (readiness backend not built)\n");
    return 0;
}
#endif
