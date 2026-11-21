// dyna-evloop interest batching (v10b-26): on kqueue, add/mod/del queue changes that ride the next
// dyn_evloop_poll kevent (one syscall per turn); an add undone in the same turn costs nothing; a
// delete+re-add after close/fd reuse becomes one EV_ADD with the new generation; EBADF on a queued add
// is dropped (the fd was closed, as when close() removed a live knote); any other add failure reaches the
// callback as DYN_EV_ERROR; dyn_evloop_forget (fd about to be closed) drops its queued changes without a
// syscall (close removes the knotes, and a queued DELETE would come back as an error wakeup); a loop whose fd was exported (dyn_evloop_backend_fd) is flushed by
// dyn_evloop_flush_pending() before an outer poller blocks on that fd.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include "dyna-evloop.h"

static int fails, checks;
static void ok(int c, const char* m) { checks++; if (!c) { fails++; printf("FAIL: %s\n", m); } }
static int hits[4096], masks[4096];
static void cb(dyn_evloop_t* lp, int fd, int ev, void* ud) { (void)lp; (void)ud; hits[fd]++; masks[fd] |= ev; }
static void cb2(dyn_evloop_t* lp, int fd, int ev, void* ud) { (void)lp; (void)ev; (void)ud; hits[fd] += 100; }
static void stats(uint64_t* c, uint64_t* ch) { dyn_evloop_thread_stats(c, ch); }
static void reset(void) { memset(hits, 0, sizeof hits); memset(masks, 0, sizeof masks); }

int main(void)
{
    dyn_evloop_t* lp = dyn_evloop_new();
    int sv[2], tv[2], i, kfd;
    uint64_t c0, h0, c1, h1;
    struct rlimit rl;
    if (!lp) { printf("FAIL: new\n"); return 1; }
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
    dyn_evloop_free(lp);
    printf("SKIP: interest batching is the kqueue backend's (epoll applies each change at once)\n");
    return 0;
#endif
    kfd = dyn_evloop_backend_fd(lp);
    dyn_evloop_free(lp);
    if (kfd < 0) { printf("SKIP: poll(2) backend has no descriptor\n"); return 0; }
    lp = dyn_evloop_new();
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < 2048) { rl.rlim_cur = rl.rlim_max < 2048 ? rl.rlim_max : 2048; setrlimit(RLIMIT_NOFILE, &rl); }

    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[1], "x", 1);
    reset(); stats(&c0, &h0);
    dyn_evloop_add(lp, sv[0], DYN_EV_READ, cb, NULL);
    stats(&c1, &h1);
    ok(c1 == c0, "add queues: no syscall before poll");
    dyn_evloop_poll(lp, 0);
    stats(&c1, &h1);
    ok(c1 - c0 == 1 && h1 - h0 == 1, "add + poll = one kevent carrying one change");
    ok(hits[sv[0]] == 1 && (masks[sv[0]] & DYN_EV_READ), "readable fd dispatched in the same call");

    reset(); stats(&c0, &h0);
    dyn_evloop_mod(lp, sv[0], DYN_EV_READ | DYN_EV_WRITE);
    dyn_evloop_mod(lp, sv[0], DYN_EV_READ);
    dyn_evloop_add(lp, sv[1], DYN_EV_READ, cb, NULL);
    dyn_evloop_del(lp, sv[1]);
    dyn_evloop_poll(lp, 0);
    stats(&c1, &h1);
    ok(c1 - c0 == 1 && h1 == h0, "write on/off and add/del in one turn cancel: zero changes submitted");
    ok(hits[sv[1]] == 0, "cancelled add never dispatches");

    {
        int fd = sv[0], nfd;
        reset();
        dyn_evloop_del(lp, fd);
        close(fd);
        socketpair(AF_UNIX, SOCK_STREAM, 0, tv);
        nfd = tv[0] == fd ? tv[0] : dup2(tv[0], fd);
        if (nfd == fd && tv[0] != fd)
            close(tv[0]);
        write(tv[1], "y", 1);
        dyn_evloop_add(lp, fd, DYN_EV_READ, cb2, NULL);
        stats(&c0, &h0);
        dyn_evloop_poll(lp, 0);
        stats(&c1, &h1);
        ok(hits[fd] == 100, "delete + close + reuse + add in one turn: new fd dispatched to the new callback");
        ok(h1 - h0 == 1, "delete + re-add collapse into one EV_ADD");
        dyn_evloop_del(lp, fd);
        close(fd);
        close(tv[1]);
    }

    {
        int a[2];
        reset();
        socketpair(AF_UNIX, SOCK_STREAM, 0, a);
        write(a[1], "z", 1);
        dyn_evloop_add(lp, a[0], DYN_EV_READ, cb, NULL);
        close(a[0]);
        ok(dyn_evloop_poll(lp, 0) >= 0, "queued add on a closed fd: poll does not fail");
        ok(hits[a[0]] == 0, "queued add on a closed fd (EBADF) is dropped silently");
        dyn_evloop_del(lp, a[0]);
        close(a[1]);
    }

    {
        dyn_evloop_t* other = dyn_evloop_new();
        int ofd = dyn_evloop_backend_fd(other);
        reset();
        dyn_evloop_add(lp, kfd >= 0 ? dyn_evloop_backend_fd(lp) : -1, DYN_EV_READ, cb, NULL);
        dyn_evloop_poll(lp, 0);
        ok(hits[dyn_evloop_backend_fd(lp)] == 1 && (masks[dyn_evloop_backend_fd(lp)] & DYN_EV_ERROR), "add the kernel refuses (not EBADF) reaches the callback as DYN_EV_ERROR");
        dyn_evloop_del(lp, dyn_evloop_backend_fd(lp));
        dyn_evloop_poll(lp, 0);
        dyn_evloop_free(other);
        (void)ofd;
    }

    {
        enum { NP = 200 };
        int p[NP][2], got = 0;
        reset();
        for (i = 0; i < NP; i++) {
            socketpair(AF_UNIX, SOCK_STREAM, 0, p[i]);
            dyn_evloop_add(lp, p[i][0], DYN_EV_READ | DYN_EV_WRITE, cb, NULL);
        }
        stats(&c0, &h0);
        dyn_evloop_poll(lp, 0);
        stats(&c1, &h1);
        for (i = 0; i < NP; i++)
            got += (masks[p[i][0]] & DYN_EV_WRITE) != 0;
        ok(h1 - h0 == 2 * NP, "400 queued changes all submitted");
        ok(got == NP, "every fd of a >256-change batch dispatched writable");
        for (i = 0; i < NP; i++) {
            dyn_evloop_del(lp, p[i][0]);
            close(p[i][0]);
            close(p[i][1]);
        }
        dyn_evloop_poll(lp, 0);
    }

    {
        struct pollfd pf;
        int b[2], kq = dyn_evloop_backend_fd(lp);
        socketpair(AF_UNIX, SOCK_STREAM, 0, b);
        write(b[1], "w", 1);
        dyn_evloop_add(lp, b[0], DYN_EV_READ, cb, NULL);
        pf.fd = kq; pf.events = POLLIN; pf.revents = 0;
        ok(poll(&pf, 1, 0) == 0, "exported loop: a queued add is not yet visible to an outer poller");
        dyn_evloop_flush_pending();
        pf.revents = 0;
        ok(poll(&pf, 1, 50) == 1, "dyn_evloop_flush_pending makes the exported fd readable");
        reset();
        dyn_evloop_poll(lp, 0);
        ok(hits[b[0]] == 1, "flushed registration dispatches on the next poll");
        dyn_evloop_del(lp, b[0]);
        close(b[0]);
        close(b[1]);
    }
    {
        int f[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, f);
        dyn_evloop_add(lp, f[0], DYN_EV_READ | DYN_EV_WRITE, cb, NULL);
        dyn_evloop_poll(lp, 0);
        dyn_evloop_mod(lp, f[0], DYN_EV_READ);
        stats(&c0, &h0);
        dyn_evloop_forget(lp, f[0]);
        close(f[0]);
        reset();
        ok(dyn_evloop_poll(lp, 0) == 0, "forget + close: nothing dispatched");
        stats(&c1, &h1);
        ok(c1 - c0 == 1 && h1 == h0, "forget drops the queued changes of a closing fd: no DELETE submitted");
        ok(dyn_evloop_count(lp) == 0, "forget unregisters");
        close(f[1]);
    }
    close(sv[1]);
    dyn_evloop_free(lp);
    printf("test_evloop_batch: %d checks, fails=%d\n", checks, fails);
    return fails ? 1 : 0;
}
