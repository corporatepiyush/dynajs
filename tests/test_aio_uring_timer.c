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

    CHECK(dyn_aio_set_timer(a, 150) == 0, "set_timer(150)");
    pfd.fd = dyn_aio_backend_fd(a);
    pfd.events = POLLIN;
    for (i = 0; i < 5; i++) {
        n = poll(&pfd, 1, 400);
        if (n > 0) {
            wakes++;
            dyn_aio_drain(a);
        }
    }
    CHECK(wakes >= 4, "the tick woke the eventfd in %d of 5 windows, want >= 4 "
          "(the dropped-CQE bug scores 1; the unsubmitted-arm bug scores 0)",
          wakes);

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
