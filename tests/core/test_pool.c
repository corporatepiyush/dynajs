#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dyn-pool.h"

#include <sched.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_work_runs, g_done_runs;
static pthread_t g_done_thread;
static int g_done_thread_set;

static void w_bump(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    g_work_runs++;
    pthread_mutex_unlock(&g_mu);
}

static void d_bump(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    g_done_runs++;
    g_done_thread = pthread_self();
    g_done_thread_set = 1;
    pthread_mutex_unlock(&g_mu);
}

static pthread_mutex_t g_block = PTHREAD_MUTEX_INITIALIZER;
static void w_block(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_block);
    pthread_mutex_unlock(&g_block);
}
static void d_noop(void* arg) { (void)arg; }

static int g_live, g_live_max;
static void w_live(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    if (++g_live > g_live_max)
        g_live_max = g_live;
    pthread_mutex_unlock(&g_mu);
    usleep(40000);
    pthread_mutex_lock(&g_mu);
    g_live--;
    pthread_mutex_unlock(&g_mu);
}

static int g_order[64], g_order_n;
static void w_order(void* arg)
{
    pthread_mutex_lock(&g_mu);
    if (g_order_n < 64)
        g_order[g_order_n++] = (int)(long)arg;
    pthread_mutex_unlock(&g_mu);
}

static int drain_until(dyn_pool_chan_t* ch, int want, int max_ms)
{
    int waited = 0, got = 0;
    while (got < want && waited < max_ms) {
        struct pollfd pfd;
        pfd.fd = dyn_pool_wake_fd(ch);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 10) > 0)
            got += dyn_pool_drain(ch);
        else
            waited += 10;
    }
    return got;
}

static long usable_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__)
    {
        cpu_set_t set;
        FILE* f;
        long q = 0, per = 0;
        if (sched_getaffinity(0, sizeof(set), &set) == 0)
            n = CPU_COUNT(&set);
        f = fopen("/sys/fs/cgroup/cpu.max", "r");
        if (f) {
            if (fscanf(f, "%ld %ld", &q, &per) == 2 && q > 0 && per > 0 && (q + per - 1) / per < n)
                n = (q + per - 1) / per;
            fclose(f);
        }
    }
#endif
    return n;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    {
        unsigned d = dyn_pool_default_threads();
        long ncpu = usable_cpus();
        unsigned want = (ncpu < 1) ? 1u : (ncpu > 256) ? 256u
                                                       : (unsigned)ncpu;
        CHECK(d == want, "default threads %u, expected the usable cpus (ncpu=%ld, ceiling 256)=%u",
            d, ncpu, want);
        CHECK(dyn_pool_max_threads() == want, "max threads %u, expected %u",
            dyn_pool_max_threads(), want);
        {
            dyn_pool_t* big = dyn_pool_new(1024, 0);
            CHECK(big && dyn_pool_threads(big) == want,
                "a 1024-thread request must clamp to %u (got %u)", want,
                dyn_pool_threads(big));
            dyn_pool_free(big);
        }
        {
            dyn_pool_t* a = dyn_pool_shared_acquire();
            dyn_pool_t* b = dyn_pool_shared_acquire();
            CHECK(a && a == b, "shared pool must be one instance");
            CHECK(dyn_pool_threads(a) == want, "shared pool threads %u, expected %u",
                dyn_pool_threads(a), want);
            CHECK(dyn_pool_shared_refs() == 2, "shared refs %u, expected 2",
                dyn_pool_shared_refs());
            dyn_pool_shared_release(a);
            dyn_pool_shared_release(b);
            CHECK(dyn_pool_shared_refs() == 0, "shared refs %u after release",
                dyn_pool_shared_refs());
        }
    }

    {
        dyn_pool_t* p = dyn_pool_new(4, 64);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        const int N = 200;
        int i, got;
        CHECK(p != NULL, "pool_new");
        if (!p)
            return 1;

        g_work_runs = g_done_runs = 0;
        g_done_thread_set = 0;
        got = 0;
        for (i = 0; i < N;) {
            if (dyn_pool_submit(ch, w_bump, d_bump, NULL) == 0) {
                i++;
                continue;
            }
            got += drain_until(ch, 1, 5000);
        }
        got += drain_until(ch, N - got, 5000);
        CHECK(got == N, "drained %d completions, expected %d", got, N);
        pthread_mutex_lock(&g_mu);
        CHECK(g_work_runs == N, "work ran %d times, expected %d", g_work_runs, N);
        CHECK(g_done_runs == N, "done ran %d times, expected %d", g_done_runs, N);
        CHECK(g_done_thread_set && pthread_equal(g_done_thread, pthread_self()),
            "done must run on the DRAINING thread, not a worker");
        pthread_mutex_unlock(&g_mu);
        CHECK(dyn_pool_inflight(ch) == 0, "inflight should be 0 after drain, is %zu",
            (size_t)dyn_pool_inflight(ch));
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    {
        dyn_pool_t* p = dyn_pool_new(2, 8);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        unsigned cap = dyn_pool_capacity(p);
        int refused = 0, accepted = 0, i;
        CHECK(p != NULL, "pool_new (small)");
        if (!p)
            return 1;

        pthread_mutex_lock(&g_block);
        for (i = 0; i < (int)cap * 4; i++) {
            if (dyn_pool_submit(ch, w_block, d_noop, NULL) == 0)
                accepted++;
            else
                refused++;
        }
        CHECK(refused == 0 && accepted == (int)cap * 4, "a full ring overflows inside the pool, nothing is refused (cap=%u, accepted=%d)",
            cap, accepted);
        pthread_mutex_unlock(&g_block);
        CHECK(drain_until(ch, accepted, 10000) == accepted, "every overflowed job completes");
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    {
        dyn_pool_t* p = dyn_pool_new(2, 8);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        unsigned cap = dyn_pool_capacity(p);
        int accepted = 0, round, i;
        CHECK(p != NULL, "pool_new (overflow)");
        if (!p)
            return 1;

        for (round = 0; round < 4; round++) {
            for (i = 0; i < (int)cap; i++)
                if (dyn_pool_submit(ch, w_bump, d_noop, NULL) == 0)
                    accepted++;
            poll(NULL, 0, 50);
        }
        CHECK(accepted == (int)cap * 4,
            "accepted %d of %d without draining (cap %u): the excess waits in overflow, the completion ring never holds more than cap",
            accepted, (int)cap * 4, cap);
        CHECK(dyn_pool_inflight(ch) == (size_t)accepted,
            "inflight %zu counts ring + overflow (%d)", (size_t)dyn_pool_inflight(ch), accepted);
        CHECK(drain_until(ch, accepted, 10000) == accepted, "all of them complete");
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    {
        dyn_pool_t* p = dyn_pool_new(2, 32);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int i;
        CHECK(p != NULL, "pool_new (teardown)");
        if (!p)
            return 1;
        pthread_mutex_lock(&g_block);
        for (i = 0; i < 20; i++)
            (void)dyn_pool_submit(ch, w_block, d_noop, NULL);
        pthread_mutex_unlock(&g_block);
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
        printf("  teardown with queued work: ok\n");
    }

    {
        dyn_pool_t* p = dyn_pool_new(2, 16);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        struct pollfd pfd;
        CHECK(p != NULL, "pool_new (wake)");
        if (!p)
            return 1;
        CHECK(dyn_pool_wake_fd(ch) >= 0, "wake fd must be valid");

        pfd.fd = dyn_pool_wake_fd(ch);
        pfd.events = POLLIN;
        pfd.revents = 0;
        CHECK(poll(&pfd, 1, 0) == 0, "wake fd must be QUIET with no work");

        g_work_runs = g_done_runs = 0;
        CHECK(dyn_pool_submit(ch, w_bump, d_bump, NULL) == 0, "submit (wake)");
        pfd.revents = 0;
        CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN),
            "wake fd must become readable after a completion");
        CHECK(dyn_pool_drain(ch) == 1, "drain should run exactly one done");
        pfd.revents = 0;
        CHECK(poll(&pfd, 1, 0) == 0, "wake fd must be quiet again after drain");
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    {
        dyn_pool_t* p = dyn_pool_new(4, 32);
        dyn_pool_chan_t* a = dyn_pool_chan_new(p);
        dyn_pool_chan_t* b = dyn_pool_chan_new(p);
        int i, ga, gb;
        CHECK(p && a && b, "pool + two channels");
        if (!p || !a || !b)
            return 1;
        CHECK(dyn_pool_wake_fd(a) != dyn_pool_wake_fd(b),
            "each channel needs its OWN wake fd");

        for (i = 0; i < 10; i++)
            dyn_pool_submit(a, w_bump, d_noop, NULL);
        for (i = 0; i < 4; i++)
            dyn_pool_submit(b, w_bump, d_noop, NULL);

        ga = drain_until(a, 10, 5000);
        gb = drain_until(b, 4, 5000);
        CHECK(ga == 10, "channel a drained %d, expected exactly its own 10", ga);
        CHECK(gb == 4, "channel b drained %d, expected exactly its own 4", gb);
        CHECK(dyn_pool_inflight(a) == 0 && dyn_pool_inflight(b) == 0,
            "both channels should be empty (a=%zu b=%zu)",
            (size_t)dyn_pool_inflight(a), (size_t)dyn_pool_inflight(b));

        for (i = 0; i < 6; i++)
            dyn_pool_submit(b, w_bump, d_noop, NULL);
        dyn_pool_chan_free(a);
        CHECK(drain_until(b, 6, 5000) == 6,
            "closing channel a must not swallow channel b's completions");
        dyn_pool_chan_free(b);
        dyn_pool_free(p);
    }

    {
        dyn_pool_t* p = dyn_pool_new(2, 16);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int before, i;
        CHECK(p && ch, "pool + channel (close)");
        if (!p || !ch)
            return 1;
        pthread_mutex_lock(&g_mu);
        before = g_done_runs;
        pthread_mutex_unlock(&g_mu);
        for (i = 0; i < 8; i++)
            dyn_pool_submit(ch, w_bump, d_bump, NULL);
        dyn_pool_chan_free(ch);
        pthread_mutex_lock(&g_mu);
        CHECK(g_done_runs == before,
            "a closing channel ran %d callbacks; a dead runtime must get none",
            g_done_runs - before);
        pthread_mutex_unlock(&g_mu);
        dyn_pool_free(p);
        printf("  close-with-inflight: ok\n");
    }

    /* One channel holding its whole budget in uncollected results must not
       stop another channel from submitting and finishing. */
    {
        dyn_pool_t* p = dyn_pool_new(2, 8);
        dyn_pool_chan_t* a = dyn_pool_chan_new(p);
        dyn_pool_chan_t* b = dyn_pool_chan_new(p);
        unsigned cap = dyn_pool_capacity(p);
        int i, ok = 0, waited = 0, start, ran = 0;
        CHECK(p && a && b, "pool + two channels (isolation)");
        if (!p || !a || !b)
            return 1;
        pthread_mutex_lock(&g_mu);
        start = g_work_runs;
        pthread_mutex_unlock(&g_mu);
        for (i = 0; i < (int)cap; i++)
            ok += dyn_pool_submit(a, w_bump, d_noop, NULL) == 0;
        CHECK(ok == (int)cap, "channel a accepted %d of its %u", ok, cap);
        while (ran < (int)cap && waited++ < 500) {
            usleep(10000);
            pthread_mutex_lock(&g_mu);
            ran = g_work_runs - start;
            pthread_mutex_unlock(&g_mu);
        }
        CHECK(ran == (int)cap, "channel a's %u jobs ran (%d)", cap, ran);
        CHECK(dyn_pool_submit(a, w_bump, d_noop, NULL) == 0,
            "channel a at its budget queues the next job in its overflow");
        CHECK(dyn_pool_submit(b, w_bump, d_noop, NULL) == 0,
            "channel b must still be accepted while a holds %u results", cap);
        CHECK(drain_until(b, 1, 5000) == 1, "channel b's job must complete");
        CHECK(drain_until(a, (int)cap + 1, 5000) == (int)cap + 1, "channel a drains its own, the overflowed job included");
        dyn_pool_chan_free(a);
        dyn_pool_chan_free(b);
        dyn_pool_free(p);
        printf("  per-channel budget: ok\n");
    }

    /* Workers take channels in turn: a late job on one channel runs within 8 jobs (POOL_BURST) of the
       other's 24-job backlog, whichever of the two was created first. A worker may stay on a channel for
       up to 8 jobs only while that channel's owner has not drained yet and channels outnumber threads,
       because then the next completion costs no wake syscall (1 thread, 4 loops: 458k -> 777k jobs/s,
       libuv 746k); strict per-job turns paid one wake write per job. */
    {
        int swap;
        for (swap = 0; swap < 2; swap++) {
            dyn_pool_t* p = dyn_pool_new(1, 32);
            dyn_pool_chan_t* first = dyn_pool_chan_new(p);
            dyn_pool_chan_t* second = dyn_pool_chan_new(p);
            dyn_pool_chan_t* busy = swap ? second : first;
            dyn_pool_chan_t* late = swap ? first : second;
            int i, before_late;
            CHECK(p && first && second, "pool + two channels (turns)");
            if (!p || !first || !second)
                return 1;
            g_order_n = 0;
            pthread_mutex_lock(&g_block);
            dyn_pool_submit_cpu(busy, w_block, d_noop, NULL);
            for (i = 0; i < 24; i++)
                dyn_pool_submit_cpu(busy, w_order, d_noop, (void*)(long)1);
            dyn_pool_submit_cpu(late, w_order, d_noop, (void*)(long)2);
            pthread_mutex_unlock(&g_block);
            drain_until(busy, 25, 5000);
            drain_until(late, 1, 5000);
            before_late = 0;
            for (i = 0; i < g_order_n && g_order[i] != 2; i++)
                before_late++;
            CHECK(g_order_n == 25, "ran %d of 25 ordered jobs", g_order_n);
            CHECK(before_late <= 8,
                "swap=%d: the late job ran after %d of the 24 queued jobs; expected at most 8",
                swap, before_late);
            dyn_pool_chan_free(first);
            dyn_pool_chan_free(second);
            dyn_pool_free(p);
        }
        printf("  channel turns: ok\n");
    }

    /* Blocking jobs never starve cpu work: with every thread holding a blocking job,
       cpu work is still admitted and completes on the spare worker. */
    {
        dyn_pool_t* p = dyn_pool_new(2, 16);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int i, ok;
        CHECK(p && ch, "pool + channel (cpu room)");
        if (!p || !ch)
            return 1;
        if (dyn_pool_threads(p) == 2) {
            pthread_mutex_lock(&g_block);
            dyn_pool_submit(ch, w_block, d_noop, NULL);
            dyn_pool_submit(ch, w_block, d_noop, NULL);
            dyn_pool_submit(ch, w_block, d_noop, NULL);
            CHECK(dyn_pool_cpu_room(ch) == 1, "room for cpu work while blocking jobs wait");
            for (ok = 0, i = 0; i < 6; i++)
                ok += dyn_pool_submit_cpu(ch, w_bump, d_noop, NULL) == 0;
            CHECK(ok == 6, "cpu jobs admitted (%d of 6)", ok);
            CHECK(drain_until(ch, 6, 5000) == 6,
                "all six cpu jobs complete while three blocking jobs are still held");
            pthread_mutex_unlock(&g_block);
            CHECK(drain_until(ch, 3, 5000) == 3, "the blocking jobs complete after release");
        } else {
            printf("  cpu room: SKIP the two-thread case (one usable core)\n");
        }
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    /* Blocking jobs beyond the threads run on elastic threads; the spare worker runs only cpu work. */
    {
        dyn_pool_t* p = dyn_pool_new(4, 32);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        int i;
        CHECK(p && ch, "pool + channel (blocking limit)");
        if (!p || !ch)
            return 1;
        g_live = g_live_max = 0;
        for (i = 0; i < 12; i++)
            dyn_pool_submit(ch, w_live, d_noop, NULL);
        CHECK(drain_until(ch, 12, 10000) == 12, "all twelve jobs complete");
        CHECK(g_live_max > (int)dyn_pool_threads(p),
            "blocking jobs beyond the threads run on the elastic tier (saw %d at once on %u threads)", g_live_max,
            dyn_pool_threads(p));
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    /* A one-thread pool has a spare too: cpu work is admitted while a blocking job holds its thread. */
    {
        dyn_pool_t* p = dyn_pool_new(1, 8);
        dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
        CHECK(p && ch, "pool + channel (one thread)");
        if (!p || !ch)
            return 1;
        CHECK(dyn_pool_cpu_room(ch) == 1, "a fresh one-thread pool has room");
        pthread_mutex_lock(&g_block);
        dyn_pool_submit(ch, w_block, d_noop, NULL);
        CHECK(dyn_pool_cpu_room(ch) == 1, "room while its thread holds a blocking job (the spare takes cpu work)");
        CHECK(dyn_pool_submit_cpu(ch, w_bump, d_noop, NULL) == 0, "cpu work is admitted while it blocks");
        CHECK(drain_until(ch, 1, 5000) == 1, "and completes on the spare");
        pthread_mutex_unlock(&g_block);
        CHECK(drain_until(ch, 1, 5000) == 1, "the blocking job completes");
        CHECK(dyn_pool_cpu_room(ch) == 1, "room returns once it is collected");
        CHECK(dyn_pool_submit_cpu(ch, w_bump, d_noop, NULL) == 0, "and cpu work is admitted again");
        CHECK(drain_until(ch, 1, 5000) == 1, "it completes");
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
        printf("  cpu room: ok\n");
    }

    if (fails == 0)
        printf("test_pool: all tests passed\n");
    else
        printf("test_pool: %d FAILED\n", fails);
    return fails != 0;
}
