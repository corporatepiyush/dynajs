/* net-9 (2026-11-21): the io_uring reactor enters the kernel once per loop turn.
 * Before, a call that queued an SQE (connect, close's cancel, recvfrom, watch/unwatch, cancel, accept,
 * a send continuation, a disk op, the tick's re-arm) entered the ring at once, and dyn_aio_run ended with
 * a trailing io_uring_submit that entered again whenever the kernel flagged task work. Counted in docker
 * with a ptrace syscall counter (tmp scripts, not a benchmark): a TCP connect+echo+close cycle cost
 * 6 io_uring_enter over 3 turns (2.0 per turn) and 16 syscalls in all; now 4 enters (1.33 per turn) and
 * 12 syscalls, the close(2) pair folded into the ring. The remaining extra enters are the bounded
 * task-work passes after a wait (test_aio_uring_taskrun case 8), taken only while the kernel flags more.
 * Now an SQE stays in the SQ ring until the turn's own enter: the wait (submit_and_wait_timeout), or in a
 * busy turn (CQEs already reaped, timeout 0) one io_uring_submit taken only when SQEs or task work are
 * pending. dyn_aio_close queues the cancel and then an IORING_OP_CLOSE, so the fd stays open until that
 * enter (its number cannot be reused before the cancel runs). The two are not linked: under DEFER_TASKRUN
 * a link's next request is issued from task work, which a plain submit does not run, so a linked close
 * waited for the next wait; unlinked, both issue inline and in order at the submit, and a failed cancel
 * still closes. Anything else that blocks submits first: dyn_evloop_flush_pending() (called by js_os_poll right
 * before poll()) runs the reactor's pre-block hook, and dyn_aio_free submits before tearing the ring down.
 * dyn_aio_run_us hands its microseconds to the wait as is (it used to round up to whole ms).
 * Pinned, with enters counted by wrapping liburing's entry points (an enter happens when SQEs are ready or
 * the kernel flags task work/overflow, and always for get_events and the wait):
 *   (1) arming a recv does not enter; the turn that delivers the data enters exactly once;
 *   (2) an SQE queued inside a callback is still queued when dyn_aio_run returns (no trailing enter), and
 *       the next turn submits it;
 *   (3) dyn_aio_close does not enter and leaves the fd open; the next turn closes it (the peer reads EOF);
 *   (4) the pre-block hook submits what is queued, as js_os_poll's poll() path needs;
 *   (5) dyn_aio_free submits a queued close (the peer reads EOF, the fd is gone);
 *   (6) dyn_aio_run_us(1500) waits 1.5 ms, not 2.
 * Mutants (each red): restore the immediate submit in dyn_aio_close (2-5); restore the trailing submit in
 * dyn_aio_run (2); drop the hook call in dyn_evloop_flush_pending (4); drop the submit in dyn_aio_free (5);
 * round dyn_aio_run_us to ms again (6). */
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include <liburing.h>

static unsigned long n_enter;
static long long last_wait_ns = -1;

static inline void note_enter(struct io_uring* r, int always)
{
    if (always || io_uring_sq_ready(r) || (IO_URING_READ_ONCE(*r->sq.kflags) & (IORING_SQ_CQ_OVERFLOW | IORING_SQ_TASKRUN)))
        n_enter++;
}
static inline int cnt_submit(struct io_uring* r)
{
    note_enter(r, 0);
    return (io_uring_submit)(r);
}
static inline int cnt_get_events(struct io_uring* r)
{
    note_enter(r, 1);
    return (io_uring_get_events)(r);
}
static inline int cnt_wait(struct io_uring* r, struct io_uring_cqe** c, unsigned n,
    struct __kernel_timespec* t, sigset_t* s)
{
    note_enter(r, 1);
    last_wait_ns = t ? (long long)t->tv_sec * 1000000000LL + t->tv_nsec : -1;
    return (io_uring_submit_and_wait_timeout)(r, c, n, t, s);
}
#define io_uring_submit(r) cnt_submit(r)
#define io_uring_get_events(r) cnt_get_events(r)
#define io_uring_submit_and_wait_timeout(r, c, n, t, s) cnt_wait(r, c, n, t, s)

#include "dyna-aio-uring.c"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>

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

typedef struct {
    int got;
    int closing_fd;
    dyn_aio_t* a;
} st_t;

static void on_recv(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n, void* ud)
{
    st_t* st = (st_t*)ud;
    (void)buf;
    if (res > 0 && n > 0)
        st->got += (int)n;
    if (st->closing_fd >= 0) {
        dyn_aio_close(a, st->closing_fd);
        st->closing_fd = -1;
    }
}

static int peer_eof(int fd, int ms)
{
    struct pollfd p;
    char c;
    p.fd = fd;
    p.events = POLLIN;
    if (poll(&p, 1, ms) <= 0)
        return 0;
    return read(fd, &c, 1) == 0;
}

static int fd_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

int main(void)
{
    dyn_aio_t* a;
    int sv[2], sw[2], sx[2], sy[2], i;
    unsigned long e0;
    st_t st;

    setvbuf(stdout, NULL, _IOLBF, 0);
    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a)
        return 1;
    if (!a->ring_wait) {
        printf("SKIP: this kernel gives no DEFER_TASKRUN+EXT_ARG ring, so the reactor waits in epoll\n");
        dyn_aio_free(a);
        return 0;
    }
    for (i = 0; i < 3; i++)
        dyn_aio_run(a, 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0, "socketpair sv");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sw) == 0, "socketpair sw");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sx) == 0, "socketpair sx");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sy) == 0, "socketpair sy");

    st.got = 0;
    st.closing_fd = -1;
    st.a = a;
    e0 = n_enter;
    CHECK(dyn_aio_recv(a, sv[0], 0, 1, on_recv, &st) == 0, "dyn_aio_recv");
    CHECK(n_enter == e0, "(1) arming a recv entered the ring %lu time(s), want 0", n_enter - e0);
    CHECK(io_uring_sq_ready(&a->ring) > 0, "(1) the recv SQE waits in the SQ ring for the turn");
    CHECK(write(sv[1], "ping", 4) == 4, "write ping");
    e0 = n_enter;
    for (i = 0; i < 5 && st.got < 4; i++)
        dyn_aio_run(a, 200);
    CHECK(st.got == 4, "(1) the recv delivered %d of 4 bytes", st.got);
    CHECK(i == 1 && n_enter - e0 == 1, "(1) the turn that delivered the data took %d run(s) and %lu enter(s), want 1 and 1", i, n_enter - e0);

    st.closing_fd = sw[0];
    CHECK(write(sv[1], "pong", 4) == 4, "write pong");
    for (i = 0; i < 5 && st.got < 8; i++)
        dyn_aio_run(a, 200);
    CHECK(st.got == 8 && st.closing_fd == -1, "(2) the callback ran and closed sw[0] (got %d)", st.got);
    CHECK(io_uring_sq_ready(&a->ring) > 0, "(2) the close queued inside the callback is still in the SQ ring after dyn_aio_run (a trailing enter submitted it)");
    CHECK(fd_open(sw[0]), "(2) sw[0] stays open until the next enter");
    e0 = n_enter;
    dyn_aio_run(a, 0);
    CHECK(n_enter - e0 == 1, "(2) the next turn submitted with %lu enter(s), want 1", n_enter - e0);
    CHECK(io_uring_sq_ready(&a->ring) == 0, "(2) the next turn emptied the SQ ring");
    CHECK(peer_eof(sw[1], 500), "(2) the peer of the closed fd reads EOF");

    e0 = n_enter;
    dyn_aio_close(a, sx[0]);
    CHECK(n_enter == e0, "(3) dyn_aio_close entered the ring %lu time(s), want 0", n_enter - e0);
    CHECK(fd_open(sx[0]), "(3) the fd stays open until the turn's enter");
    dyn_aio_run(a, 0);
    CHECK(!fd_open(sx[0]) && errno == EBADF, "(3) the turn's enter closed the fd");
    CHECK(peer_eof(sx[1], 500), "(3) the peer reads EOF");

    dyn_aio_close(a, sy[0]);
    CHECK(io_uring_sq_ready(&a->ring) > 0, "(4) the close is queued");
    dyn_evloop_flush_pending();
    CHECK(io_uring_sq_ready(&a->ring) == 0, "(4) the pre-block hook submitted the queued close");
    CHECK(peer_eof(sy[1], 500), "(4) the peer reads EOF without another dyn_aio_run");

    for (i = 0; i < 3; i++)
        dyn_aio_run(a, 0);
    last_wait_ns = -2;
    dyn_aio_run_us(a, 1500);
    CHECK(last_wait_ns == 1500000LL, "(6) dyn_aio_run_us(1500) waited with a %lld ns timeout, want 1500000", last_wait_ns);

    dyn_aio_close(a, sv[0]);
    CHECK(fd_open(sv[0]), "(5) the close is queued, the fd still open");
    dyn_aio_free(a);
    CHECK(!fd_open(sv[0]), "(5) dyn_aio_free submitted the queued close");
    CHECK(peer_eof(sv[1], 500), "(5) the peer reads EOF after dyn_aio_free");

    close(sv[1]);
    close(sw[1]);
    close(sx[1]);
    close(sy[1]);
    if (fails == 0)
        printf("test_aio_uring_one_enter: all tests passed\n");
    else
        printf("test_aio_uring_one_enter: %d FAILED\n", fails);
    return fails != 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_aio_uring_one_enter: skipped (needs Linux + CONFIG_IO_URING)\n");
    return 0;
}
#endif
