// net-8 (2026-11-21): interest flips inside one turn on a knote the kernel already holds. kqueue batches
// interest changes into the next kevent call, and a queued change for the same (fd, filter) is rewritten
// in place: a read interest that was committed, then dropped (pause), restored (resume) and dropped again
// in one turn used to lose its EV_DELETE. The pending DELETE was overwritten by the ADD, and the second
// drop then threw the pending entry away as "an add undone in the same turn", which is only right when
// the kernel never had the knote. The kernel kept a level-triggered read knote the loop no longer wanted:
// every poll returned at once (the paused connection spun the loop at 100% CPU and its data or EOF/RST
// was reported to nobody), and the wrong generation tag stayed on the knote. Now each (fd, filter) keeps
// whether the kernel holds the knote, and a drop of a pending ADD over a held knote becomes an EV_DELETE.
// Pinned on every backend: after any flip sequence ending in "not interested", a pending readable fd
// neither dispatches nor wakes the poll (it waits out its timeout); after any sequence ending in
// "interested", it dispatches exactly once per poll with READ; and an add undone in the same turn on a
// fresh fd still costs no change on kqueue.
// Mutant (red): the drop path back to unconditional (the stale-knote check: poll returns at once).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "dyna-evloop.h"

static int fails, checks;
static void ok(int c, const char* m) { checks++; if (!c) { fails++; printf("FAIL: %s\n", m); } }
static int hits, mask;
static void cb(dyn_evloop_t* lp, int fd, int ev, void* ud) { (void)lp; (void)fd; (void)ud; hits++; mask |= ev; }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void check_seq(const char* name, const int* seq, int n)
{
    dyn_evloop_t* lp = dyn_evloop_new();
    int sv[2], i, want = seq[n - 1];
    char msg[256];
    double t0, el;
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    dyn_evloop_add(lp, sv[0], DYN_EV_READ, cb, NULL);
    dyn_evloop_poll(lp, 0);
    write(sv[1], "x", 1);
    for (i = 0; i < n; i++)
        dyn_evloop_mod(lp, sv[0], seq[i] ? DYN_EV_READ : 0);
    hits = 0;
    mask = 0;
    t0 = now_ms();
    dyn_evloop_poll(lp, 60);
    el = now_ms() - t0;
    if (want) {
        snprintf(msg, sizeof msg, "%s: ends interested: one READ dispatch (hits %d mask %d)", name, hits, mask);
        ok(hits == 1 && (mask & DYN_EV_READ), msg);
    } else {
        snprintf(msg, sizeof msg, "%s: ends paused: no dispatch (%d)", name, hits);
        ok(hits == 0, msg);
        snprintf(msg, sizeof msg, "%s: ends paused: the poll waits its 60 ms timeout instead of waking on a stale knote (%.1f ms)", name, el);
        ok(el >= 45, msg);
        dyn_evloop_poll(lp, 0);
        t0 = now_ms();
        dyn_evloop_poll(lp, 30);
        el = now_ms() - t0;
        snprintf(msg, sizeof msg, "%s: and on the next turn too (%.1f ms)", name, el);
        ok(el >= 20 && hits == 0, msg);
        dyn_evloop_mod(lp, sv[0], DYN_EV_READ);
        hits = 0;
        dyn_evloop_poll(lp, 60);
        snprintf(msg, sizeof msg, "%s: resuming later dispatches the pending data (%d)", name, hits);
        ok(hits == 1, msg);
    }
    dyn_evloop_del(lp, sv[0]);
    close(sv[0]);
    close(sv[1]);
    dyn_evloop_free(lp);
}

int main(void)
{
    static const int s1[] = { 0, 1, 0 }, s2[] = { 0, 1, 0, 1, 0 }, s3[] = { 0, 1 }, s4[] = { 0 }, s5[] = { 0, 1, 0, 1 },
                     s6[] = { 1, 0, 1, 0 };
    check_seq("pause,resume,pause", s1, 3);
    check_seq("pause,resume,pause,resume,pause", s2, 5);
    check_seq("pause,resume", s3, 2);
    check_seq("pause", s4, 1);
    check_seq("pause,resume,pause,resume", s5, 4);
    check_seq("noop,pause,resume,pause", s6, 4);
    {
        dyn_evloop_t* lp = dyn_evloop_new();
        int sv[2];
        uint64_t c0, h0, c1, h1;
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        dyn_evloop_thread_stats(&c0, &h0);
        dyn_evloop_add(lp, sv[0], DYN_EV_READ, cb, NULL);
        dyn_evloop_mod(lp, sv[0], 0);
        dyn_evloop_mod(lp, sv[0], DYN_EV_READ);
        dyn_evloop_mod(lp, sv[0], 0);
        dyn_evloop_poll(lp, 0);
        dyn_evloop_thread_stats(&c1, &h1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        ok(h1 == h0, "kqueue, fresh fd: add/del flips in one turn submit no change");
#else
        (void)c0; (void)c1; (void)h0; (void)h1;
#endif
        close(sv[0]);
        close(sv[1]);
        dyn_evloop_free(lp);
    }
    printf("test_evloop_interest_flip: %d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
