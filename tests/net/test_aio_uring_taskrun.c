/* The io_uring reactor runs its completion task work only when it asks for it (DEFER_TASKRUN).
 * Without that flag the kernel ran every recv/send completion as task work that interrupted the
 * reactor thread wherever it was (arch_do_signal_or_restart 14-17% of server+client CPU, plus the
 * wake-up lock contention): a JS TCP echo, 16 KiB x 32 in flight, ran at x0.65 of epoll, 64 B x 32
 * at x0.42. With DEFER_TASKRUN + TASKRUN_FLAG the work waits until the reactor's loop calls
 * io_uring_get_events, which it does only when the kernel flags pending work (IORING_SQ_TASKRUN),
 * so an idle pass costs no extra syscall.
 * Pinned here: (1) a kernel that accepts the flags gets them on the reactor's ring; (2) data that
 * arrives while the quiet reactor sleeps is delivered by the very pass that woke for it, both through
 * dyn_aio_run and through the embedding pattern (poll the backend fd, then one dyn_aio_drain). A pass
 * that skipped the deferred work after its wait returned empty-handed and left the completion for a
 * later pass (liburing's next submit flushes it, one loop turn late).
 * The reactor then sleeps in io_uring_enter itself (io_uring_submit_and_wait_timeout) instead of in
 * epoll_pwait on its eventfd, so the deferred work runs inside the wait syscall: at depth 1 a request
 * cost sendto + epoll_pwait + read(eventfd) + io_uring_enter and now costs sendto + io_uring_enter.
 * poll() on a DEFER_TASKRUN ring fd is never woken by new work (probe, kernel 7.1: 0 of 20 wakes,
 * every poll returned at its timeout), so the embedder's backend fd stays the eventfd's epoll set;
 * the eventfd keeps counting while the reactor sleeps in the ring and the first poll()-mode pass
 * drains it. Also pinned: (3) a kernel with DEFER_TASKRUN and EXT_ARG gets the in-ring wait; (4) the
 * wait honours its timeout (a run with nothing pending returns after ~timeout_ms, not at the next
 * event), which the JS timers, setTimeout precision and --timeout-ms depend on; (5) an fd a module
 * adds to dyn_aio_evloop() (Spawn's pipes, file watchers) still wakes the reactor while it sleeps in the
 * ring: the ring holds a multishot poll on that loop's epoll fd, and the eventfd lives in a separate outer
 * epoll (the embedder's backend fd), because a poll that saw the eventfd would fire on every CQE it posts.
 * (6) that loop stays level-triggered: the ring's poll on its epoll fd fires once per wake-up, so a
 * callback that leaves data unread (Spawn reads only what JS asked for) must still be called again on
 * the next pass; the reactor re-polls that loop until a pass finds nothing ready (test_spawn's streams
 * and test_spawn_lifetime's 32 reads hung without it).
 * (7) the eventfd is never in a set the ring polls. If it were, each CQE would signal the eventfd, wake
 * the ring's poll, post a CQE, signal again: a livelock inside the kernel's deferred task work that no
 * user-space cap or timeout interrupts (it wedged the test VM once). So this is checked structurally,
 * first thing, before the ring is ever entered: epoll_ctl(ADD) of the eventfd into the polled set must
 * succeed (it is absent) and into the outer set must fail with EEXIST (it is there); on a violation the
 * test exits at once without running the reactor.
 * (8) one pass delivers what the sockets already hold: a wait with min_complete=1 runs only a slice of
 * the deferred task work (the kernel caps it), so the pass that woke used to return with 9-12 of 32
 * queued datagrams and left the rest for later passes; a JS timer due in between (metrics-pipeline's
 * sleep(2) before its flush) then ran first and the flush counted 14 or 50 users instead of ~40.
 * After the wait the reactor now keeps running task work while the kernel flags more (bounded passes):
 * 48 datagrams queued on an idle socket must all arrive in the one dyn_aio_run that wakes for them.
 * Compiled with dyna-aio-uring.c included so the ring's setup flags can be read. */
#include "dyna-aio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_IO_URING) && defined(__linux__)

#include "dyna-aio-uring.c"

#include <poll.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>

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

static size_t got_bytes;
static int got_calls;

static void on_recv(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n, void* ud)
{
    (void)a;
    (void)buf;
    (void)ud;
    if (res > 0) {
        got_bytes += n;
        got_calls++;
    }
}

static int late_fd;
static volatile int late_written;

static void* late_writer(void* arg)
{
    struct timespec ts = { 0, 30 * 1000 * 1000 };
    char m[64];
    (void)arg;
    memset(m, 'w', sizeof(m));
    nanosleep(&ts, NULL);
    if (write(late_fd, m, sizeof(m)) != (ssize_t)sizeof(m))
        return (void*)1;
    __atomic_store_n(&late_written, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void on_alarm(int sig)
{
    (void)sig;
}

static int user_fd_hits;
static void on_user_fd(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    char b[256];
    (void)lp;
    (void)events;
    (void)ud;
    if (read(fd, b, sizeof(b)) > 0)
        user_fd_hits++;
}

static int partial_hits;
static void on_partial_fd(dyn_evloop_t* lp, int fd, int events, void* ud)
{
    char b[16];
    (void)lp;
    (void)events;
    (void)ud;
    if (read(fd, b, sizeof(b)) > 0)
        partial_hits++;
}

static int dg_got;
static void on_dgram(dyn_aio_t* a, int res, const uint8_t* buf, unsigned n, const struct sockaddr* sa,
    unsigned salen, void* ud)
{
    (void)a;
    (void)buf;
    (void)n;
    (void)sa;
    (void)salen;
    (void)ud;
    if (res >= 0)
        dg_got++;
}

static int kernel_takes_defer(void)
{
    struct io_uring r;
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    if (io_uring_queue_init_params(8, &r, &p) < 0)
        return 0;
    io_uring_queue_exit(&r);
    return 1;
}

static void settle(dyn_aio_t* a, size_t want)
{
    struct pollfd q;
    int k;
    for (k = 0; k < 20 && got_bytes < want; k++)
        dyn_aio_run(a, 100);
    q.fd = dyn_aio_backend_fd(a);
    q.events = POLLIN;
    for (k = 0; k < 20; k++) {
        if (poll(&q, 1, 0) == 0 && io_uring_cq_ready(&a->ring) == 0)
            break;
        dyn_aio_drain(a);
    }
}

int main(void)
{
    dyn_aio_t* a;
    int sv[2], i, defer = kernel_takes_defer();
    struct pollfd pfd;
    size_t want = 0;
    int empty_after_data = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
    }
    a = dyn_aio_new(0, 0);
    CHECK(a != NULL, "dyn_aio_new");
    if (!a)
        return 1;
    {
        struct epoll_event ev;
        int inner, outer;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        inner = epoll_ctl(dyn_evloop_backend_fd(a->evl), EPOLL_CTL_ADD, a->evfd, &ev);
        if (inner == 0)
            epoll_ctl(dyn_evloop_backend_fd(a->evl), EPOLL_CTL_DEL, a->evfd, NULL);
        outer = epoll_ctl(dyn_evloop_backend_fd(a->outer), EPOLL_CTL_ADD, a->evfd, &ev);
        if (outer == 0)
            epoll_ctl(dyn_evloop_backend_fd(a->outer), EPOLL_CTL_DEL, a->evfd, NULL);
        if (inner != 0 || outer == 0 || errno != EEXIST) {
            printf("FAIL: the ring's eventfd must sit only in the outer set (ring-polled set: %s, outer set: %s); "
                   "not entering the ring, a ring-polled eventfd livelocks the kernel\n",
                inner == 0 ? "absent" : "PRESENT", outer == 0 ? "ABSENT" : "present");
            printf("test_aio_uring_taskrun: 1 FAILED\n");
            return 1;
        }
    }
    if (defer)
        CHECK((a->ring.flags & IORING_SETUP_DEFER_TASKRUN) && (a->ring.flags & IORING_SETUP_TASKRUN_FLAG),
            "the kernel takes DEFER_TASKRUN|TASKRUN_FLAG but the reactor ring has flags 0x%x", a->ring.flags);
    else
        printf("test_aio_uring_taskrun: kernel refuses DEFER_TASKRUN; flag check skipped, delivery still checked\n");

    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0, "socketpair");
    CHECK(dyn_aio_recv(a, sv[0], 0, 1, on_recv, NULL) == 0, "dyn_aio_recv multishot");
    late_fd = sv[1];
    pfd.fd = dyn_aio_backend_fd(a);
    pfd.events = POLLIN;

    settle(a, want);
    for (i = 0; i < 10; i++) {
        pthread_t th;
        int p;
        want += 64;
        late_written = 0;
        CHECK(pthread_create(&th, NULL, late_writer, NULL) == 0, "pthread_create");
        for (p = 0; p < 10 && got_bytes < want; p++) {
            dyn_aio_run(a, 2000);
            if (got_bytes < want && __atomic_load_n(&late_written, __ATOMIC_SEQ_CST))
                empty_after_data++;
        }
        pthread_join(th, NULL);
        settle(a, want);
    }
    CHECK(empty_after_data <= 2, "%d of 10 dyn_aio_run passes returned empty-handed after the data had arrived "
                                 "(want <= 2: the pass that wakes for it delivers it)",
        empty_after_data);
    for (i = 0; i < 10; i++) {
        pthread_t th;
        size_t before = got_bytes;
        int pr;
        want += 64;
        CHECK(pthread_create(&th, NULL, late_writer, NULL) == 0, "pthread_create");
        pr = poll(&pfd, 1, 2000);
        CHECK(pr == 1, "round %d: the backend fd woke for the data (poll %d)", i, pr);
        dyn_aio_drain(a);
        pthread_join(th, NULL);
        CHECK(got_bytes == want, "round %d: the one dyn_aio_drain after the wake delivered %zu of 64 bytes",
            i, got_bytes - before);
        settle(a, want);
    }
    CHECK(got_bytes == want, "all %zu bytes arrived in the end (%zu)", want, got_bytes);

    if (defer && (a->ring.features & IORING_FEAT_EXT_ARG))
        CHECK(a->ring_wait, "DEFER_TASKRUN + EXT_ARG ring, but the reactor still sleeps in epoll_pwait");
    for (i = 0; i < 3; i++) {
        double t0 = now_ms(), dt;
        int ms = 60 + 40 * i;
        alarm(3);
        dyn_aio_run(a, ms);
        alarm(0);
        dt = now_ms() - t0;
        CHECK(dt >= ms - 5 && dt < ms + 250, "dyn_aio_run(%d) with nothing pending returned after %.1f ms", ms, dt);
    }

    {
        int pp[2];
        CHECK(pipe2(pp, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
        CHECK(dyn_evloop_add(dyn_aio_evloop(a), pp[0], DYN_EV_READ, on_user_fd, NULL) == 0, "evloop add");
        for (i = 0; i < 5; i++) {
            pthread_t th;
            double t0 = now_ms(), dt;
            int hits = user_fd_hits, k;
            late_fd = pp[1];
            CHECK(pthread_create(&th, NULL, late_writer, NULL) == 0, "pthread_create");
            for (k = 0; k < 5 && user_fd_hits == hits; k++)
                dyn_aio_run(a, 1000);
            pthread_join(th, NULL);
            dt = now_ms() - t0;
            CHECK(user_fd_hits == hits + 1 && dt < 500,
                "round %d: a pipe on dyn_aio_evloop() woke the sleeping reactor after %.1f ms (hits %d)", i, dt,
                user_fd_hits - hits);
        }
        dyn_evloop_del(dyn_aio_evloop(a), pp[0]);
        close(pp[0]);
        close(pp[1]);
    }
    {
        int pp[2], k;
        double t0, dt;
        char m[64];
        memset(m, 'p', sizeof(m));
        CHECK(pipe2(pp, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
        CHECK(dyn_evloop_add(dyn_aio_evloop(a), pp[0], DYN_EV_READ, on_partial_fd, NULL) == 0, "evloop add");
        CHECK(write(pp[1], m, sizeof(m)) == (ssize_t)sizeof(m), "write");
        t0 = now_ms();
        for (k = 0; k < 10 && partial_hits < 4; k++)
            dyn_aio_run(a, 1000);
        dt = now_ms() - t0;
        CHECK(partial_hits == 4 && dt < 500,
            "a callback that reads 16 of 64 ready bytes is called again until they are gone: %d of 4 reads in %.1f ms",
            partial_hits, dt);
        dyn_evloop_del(dyn_aio_evloop(a), pp[0]);
        close(pp[0]);
        close(pp[1]);
    }

    {
        int rfd = dyn_aio_udp_bind(a, "127.0.0.1", 0), sfd = socket(AF_INET, SOCK_DGRAM, 0), rcv = 4 << 20, k, round;
        struct sockaddr_in to;
        socklen_t tl = sizeof(to);
        CHECK(rfd >= 0 && sfd >= 0, "udp sockets");
        setsockopt(rfd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        getsockname(rfd, (struct sockaddr*)&to, &tl);
        CHECK(dyn_aio_recvfrom(a, rfd, on_dgram, NULL) == 0, "dyn_aio_recvfrom");
        settle(a, want);
        for (round = 0; round < 5; round++) {
            dg_got = 0;
            for (k = 0; k < 48; k++)
                sendto(sfd, "dgram", 5, 0, (struct sockaddr*)&to, sizeof(to));
            dyn_aio_run(a, 1000);
            CHECK(dg_got == 48, "round %d: the pass that woke delivered %d of 48 queued datagrams", round, dg_got);
            for (k = 0; k < 20 && dg_got < 48; k++)
                dyn_aio_run(a, 100);
        }
        dyn_aio_close(a, rfd);
        close(sfd);
    }

    dyn_aio_close(a, sv[0]);
    close(sv[1]);
    dyn_aio_free(a);
    if (fails == 0)
        printf("test_aio_uring_taskrun: all tests passed (%d recv completions, defer=%d)\n", got_calls, defer);
    else
        printf("test_aio_uring_taskrun: %d FAILED\n", fails);
    return fails != 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_aio_uring_taskrun: skipped (needs Linux + CONFIG_IO_URING)\n");
    return 0;
}
#endif
