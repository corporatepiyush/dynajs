/* test_aio_uring_timer.c -- the io_uring tick timer must fire through the
 * EVENTFD on a quiet loop, with NO fd ever registered (fds table at cap == 0).
 *
 * Two bugs lived here, both invisible to any test that pumps with
 * dyn_aio_run (entering the ring submits and reaps regardless):
 *  1. uaio_dispatch checked `fd >= a->cap` before the UOP_TIMER branch, but
 *     the timer is keyed by tfd and owns no slot -- with the arm preceding
 *     any fd_ensure, every tick CQE was dropped and the read re-armed at
 *     most ONCE.
 *  2. dyn_aio_set_timer queued the timerfd read SQE without SUBMITTING it;
 *     on a quiet loop nothing else enters the ring, so the read never armed.
 * Measured: poll(backend_fd) woke 0/6 windows before both fixes, 6/6 after.
 * Reproduced live: a PostgreSQL connect to a blackhole with
 * connectTimeoutMs: 500 hung forever -- every dyn_net_on_drain deadline
 * (pg/tcp timeouts, idle sweeps) was dead on arrival.
 */

#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <poll.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    dyn_aio_t *a;
    struct pollfd pfd;
    int i, wakes = 0, n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a)
        return 1;

    /* NO socket is ever opened: cap stays 0, which is exactly the state the
     * CQE drop lived in. The poll+drain shape is what js_std_loop does -- the
     * engine never calls dyn_aio_run on this backend. */
    CHECK(dyn_aio_set_timer(a, 150) == 0, "set_timer(150)");
    pfd.fd = dyn_aio_backend_fd(a);
    pfd.events = POLLIN;
    for (i = 0; i < 5; i++) {
        n = poll(&pfd, 1, 400);
        if (n > 0) {
            wakes++;
            dyn_aio_drain(a);       /* re-arms from the completion */
        }
    }
    CHECK(wakes >= 4, "the tick woke the eventfd in %d of 5 windows, want >= 4 "
          "(the dropped-CQE bug scores 1; the unsubmitted-arm bug scores 0)",
          wakes);

    /* Disarm is a real disarm: no stray re-arm keeps the ring waking. */
    CHECK(dyn_aio_set_timer(a, 0) == 0, "set_timer(0)");
    n = poll(&pfd, 1, 450);
    CHECK(n == 0, "after disarm the timer is silent (poll got %d)", n);

    dyn_aio_free(a);
    if (fails == 0) printf("test_aio_uring_timer: all tests passed\n");
    else printf("test_aio_uring_timer: %d FAILED\n", fails);
    return fails != 0;
}

#else
int main(void)
{
    printf("test_aio_uring_timer: skipped (needs Linux + CONFIG_IO_URING)\n");
    return 0;
}
#endif
