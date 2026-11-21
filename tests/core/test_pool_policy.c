/*
 * tests/core/test_pool_policy.c -- the review fixes of 2026-11-27 (owner: "do the fixes right now"), built with
 * -DPOOL_ELASTIC_MAX=8 -DPOOL_ELASTIC_KEEP_NS=200000000 so the cap and the idle retirement are reachable in a test:
 *   elastic  blocking jobs beyond the N regular threads run on extra threads once the class makes no progress for
 *            POOL_BLOCK_NS (1 ms; spawning on mere saturation cut 0-1 us blocking jobs by 45-80% in bench_pool) (none existed: N hung blocking calls
 *            stalled every other blocking job and left cpu work one spare core)
 *   cap      at most N + POOL_ELASTIC_MAX blocking jobs at once; the rest wait and complete after release
 *   retire   an idle elastic thread exits after POOL_ELASTIC_KEEP_NS; the tier is re-entered after retirement
 *   one      a one-thread pool admits cpu work while its thread holds a blocking job (it was EBUSY, and dyna-aio
 *            then ran the work inline on the JS thread)
 *   stall    the "progressing class spawns nothing" bound adds 1 + gap/cooldown per completion gap >= 1 ms, so a
 *            preempted or ASan-slowed run is not red; under san the bound is loose (spawn-on-saturation mutant
 *            POOL_BLOCK_NS=1 is red in release: spawned 8). Tighten only with a deterministic clock.
 *   overflow submit never refuses for capacity: jobs past the ring wait in per-class FIFO overflow inside the
 *            pool (dyna-aio kept one malloc'd queue for both classes, where a stuck io job held back cpu jobs)
 *   hol      a full io class does not hold back cpu jobs
 *   cancel   a queued job (ring or overflow) can be cancelled: work is skipped, done still runs on the owner
 *   signals  every pool thread (regular, spare, elastic) blocks all signals
 *   stack    pool threads get an explicit stack (1 MiB, 8 MiB under sanitizers), not the platform default
 *   fork     after fork() a pool from the parent refuses with ECHILD and the child gets a fresh shared pool
 *   tiny     200 us blocking jobs that keep completing spawn at most 2 elastic threads, plus what the observed stalls
 *            allow: each gap >= POOL_BLOCK_NS between two completions (a starved host, ASan, a parallel gate) may
 *            legitimately trigger 1 + gap / POOL_SPAWN_COOLDOWN_NS spawns; on a quiet host the bound stays 2
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dyn-pool.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef POOL_ELASTIC_MAX
#define POOL_ELASTIC_MAX 64u
#endif
#ifndef POOL_SPAWN_COOLDOWN_NS
#define POOL_SPAWN_COOLDOWN_NS 2000000LL
#endif
#ifndef POOL_CPU_CAP_MIN
#define POOL_CPU_CAP_MIN 8u
#endif

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
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_open;
static _Atomic int g_entered, g_ran, g_done;
static _Atomic int g_sig_bad, g_stack_bad;

static void gate(int open)
{
    pthread_mutex_lock(&g_mu);
    g_open = open;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void probe_thread(void)
{
    sigset_t cur;
    pthread_sigmask(SIG_SETMASK, NULL, &cur);
    if (!sigismember(&cur, SIGINT) || !sigismember(&cur, SIGTERM) || !sigismember(&cur, SIGCHLD)
        || !sigismember(&cur, SIGUSR1))
        atomic_fetch_add(&g_sig_bad, 1);
#if defined(__APPLE__)
    if (pthread_get_stacksize_np(pthread_self()) < (size_t)1 << 20)
        atomic_fetch_add(&g_stack_bad, 1);
#elif defined(__linux__)
    {
        pthread_attr_t at;
        size_t sz = 0;
        if (pthread_getattr_np(pthread_self(), &at) == 0) {
            pthread_attr_getstacksize(&at, &sz);
            pthread_attr_destroy(&at);
        }
        if (sz < (size_t)1 << 20 || sz > (size_t)16 << 20)
            atomic_fetch_add(&g_stack_bad, 1);
    }
#endif
}

static void w_hold(void* arg)
{
    (void)arg;
    probe_thread();
    atomic_fetch_add(&g_entered, 1);
    pthread_mutex_lock(&g_mu);
    while (!g_open)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_ran, 1);
}

static void w_mark(void* arg)
{
    probe_thread();
    atomic_fetch_or((_Atomic int*)arg, 1);
    atomic_fetch_add(&g_ran, 1);
}

static void d_count(void* arg)
{
    (void)arg;
    atomic_fetch_add(&g_done, 1);
}

static int g_seq[4096], g_seq_n;
static void d_seq(void* arg)
{
    if (g_seq_n < 4096)
        g_seq[g_seq_n++] = (int)(long)arg;
    atomic_fetch_add(&g_done, 1);
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

static int wait_entered(int want, int max_ms)
{
    int i;
    for (i = 0; i < max_ms && atomic_load(&g_entered) < want; i++)
        usleep(1000);
    return atomic_load(&g_entered);
}

static void reset(void)
{
    atomic_store(&g_entered, 0);
    atomic_store(&g_ran, 0);
    atomic_store(&g_done, 0);
    g_seq_n = 0;
    gate(0);
}

static void case_elastic_cap_retire(void)
{
    dyn_pool_t* p = dyn_pool_new(4, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    int i, n = 4 + (int)POOL_ELASTIC_MAX + 6, e;
    CHECK(p && ch, "pool + chan (elastic)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < n; i++)
        CHECK(dyn_pool_submit(ch, w_hold, d_count, NULL) == 0, "blocking job %d admitted", i);
    e = wait_entered(4 + (int)POOL_ELASTIC_MAX, 5000);
    CHECK(e == 4 + (int)POOL_ELASTIC_MAX, "%d blocking jobs ran at once on 4 threads; expected 4 + %u elastic",
        e, POOL_ELASTIC_MAX);
    usleep(100000);
    CHECK(atomic_load(&g_entered) == 4 + (int)POOL_ELASTIC_MAX, "the elastic cap holds: %d entered, cap %u",
        atomic_load(&g_entered), 4 + POOL_ELASTIC_MAX);
    dyn_pool_stats(p, &st);
    CHECK(st.blocking_live == POOL_ELASTIC_MAX && st.blocking_peak == POOL_ELASTIC_MAX,
        "blocking live %u peak %u, expected %u", st.blocking_live, st.blocking_peak, POOL_ELASTIC_MAX);
    gate(1);
    CHECK(drain_until(ch, n, 10000) == n, "all %d blocking jobs complete after release", n);
    CHECK(atomic_load(&g_sig_bad) == 0, "%d pool threads had a signal unblocked", atomic_load(&g_sig_bad));
    CHECK(atomic_load(&g_stack_bad) == 0, "%d pool threads had an unexpected stack size", atomic_load(&g_stack_bad));
    for (i = 0; i < 300; i++) {
        dyn_pool_stats(p, &st);
        if (st.blocking_live == 0)
            break;
        usleep(10000);
    }
    CHECK(st.blocking_live == 0, "idle elastic threads retire (still %u live after 3 s)", st.blocking_live);
    CHECK(st.blocking_retired == st.blocking_spawned, "retired %llu of %llu spawned", st.blocking_retired,
        st.blocking_spawned);
    reset();
    for (i = 0; i < 6; i++)
        dyn_pool_submit(ch, w_hold, d_count, NULL);
    CHECK(wait_entered(6, 5000) == 6, "the tier is re-entered after retirement (%d of 6 at once)",
        atomic_load(&g_entered));
    gate(1);
    CHECK(drain_until(ch, 6, 10000) == 6, "and those complete");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  elastic tier, cap %u, retirement: ok\n", POOL_ELASTIC_MAX);
}

static void case_one_thread(void)
{
    dyn_pool_t* p = dyn_pool_new(1, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    _Atomic int hit = 0;
    dyn_pool_stats_t st;
    CHECK(p && ch, "pool + chan (one thread)");
    if (!p || !ch)
        return;
    dyn_pool_stats(p, &st);
    CHECK(st.threads == 2, "a one-thread pool runs its thread + the spare (%u)", st.threads);
    reset();
    CHECK(dyn_pool_submit(ch, w_hold, d_count, NULL) == 0, "blocking job admitted");
    CHECK(wait_entered(1, 5000) == 1, "it runs");
    CHECK(dyn_pool_cpu_room(ch) == 1, "cpu room while the only thread blocks");
    CHECK(dyn_pool_submit_cpu(ch, w_mark, d_count, (void*)&hit) == 0, "cpu work admitted (was EBUSY)");
    CHECK(drain_until(ch, 1, 5000) == 1 && atomic_load(&hit), "and it completes on the spare while the thread blocks");
    CHECK(dyn_pool_submit(ch, w_hold, d_count, NULL) == 0 && wait_entered(2, 5000) == 2,
        "a second blocking job runs on the elastic tier");
    gate(1);
    CHECK(drain_until(ch, 2, 5000) == 2, "the blocking jobs complete");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  one-thread pool: cpu work never refused: ok\n");
}

static void case_overflow_fifo(void)
{
    dyn_pool_t* p = dyn_pool_new(1, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned cap = dyn_pool_capacity(p);
    dyn_pool_stats_t st;
    int i, n = 2000, ok = 1;
    CHECK(p && ch, "pool + chan (overflow)");
    if (!p || !ch)
        return;
    reset();
    CHECK(dyn_pool_submit_cpu(ch, w_hold, d_count, NULL) == 0 && wait_entered(1, 5000) == 1, "the thread is held");
    for (i = 0; i < n; i++)
        CHECK(dyn_pool_submit_cpu(ch, NULL, d_seq, (void*)(long)i) == 0, "cpu job %d admitted past cap %u", i, cap);
    CHECK(dyn_pool_inflight(ch) == (size_t)n + 1, "inflight counts the overflow (%zu)", dyn_pool_inflight(ch));
    dyn_pool_stats(p, &st);
    CHECK(st.overflowed >= (unsigned long long)(n - (int)cap) && st.overflow_peak >= (unsigned long long)(n - (int)cap),
        "overflowed %llu peak %llu (n %d cap %u)", st.overflowed, st.overflow_peak, n, cap);
    gate(1);
    CHECK(drain_until(ch, n + 1, 10000) == n + 1, "every job completes");
    CHECK(g_seq_n == n, "saw %d of %d ordered completions", g_seq_n, n);
    for (i = 0; i < g_seq_n; i++)
        if (g_seq[i] != i) {
            ok = 0;
            break;
        }
    CHECK(ok, "one thread completes the class in submit order (first break at %d)", i);
    CHECK(dyn_pool_inflight(ch) == 0, "inflight 0 after drain (%zu)", dyn_pool_inflight(ch));
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  overflow: never refused, FIFO, counted: ok\n");
}

static void case_no_hol(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    int i, nio = 2 + (int)POOL_ELASTIC_MAX + 40;
    _Atomic int hit = 0;
    CHECK(p && ch, "pool + chan (hol)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < nio; i++)
        dyn_pool_submit(ch, w_hold, d_count, NULL);
    wait_entered(2 + (int)POOL_ELASTIC_MAX, 5000);
    CHECK(dyn_pool_submit_cpu(ch, w_mark, d_count, (void*)&hit) == 0, "cpu job admitted behind %d io jobs", nio);
    CHECK(drain_until(ch, 1, 5000) == 1 && atomic_load(&hit), "the cpu job completes while the io class is stuck");
    gate(1);
    CHECK(drain_until(ch, nio, 10000) == nio, "the io jobs complete after release");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  a stuck io class does not hold back cpu jobs: ok\n");
}

static void case_cancel(void)
{
    dyn_pool_t* p = dyn_pool_new(1, 8);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    unsigned cap = dyn_pool_capacity(p);
    static _Atomic int flags[64];
    dyn_pool_stats_t st;
    int i, n = (int)cap + 20;
    CHECK(p && ch, "pool + chan (cancel)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 64; i++)
        atomic_store(&flags[i], 0);
    CHECK(dyn_pool_submit_cpu(ch, w_hold, d_count, NULL) == 0 && wait_entered(1, 5000) == 1, "the thread is held");
    for (i = 0; i < n; i++)
        dyn_pool_submit_cpu(ch, w_mark, d_count, (void*)&flags[i]);
    CHECK(dyn_pool_cancel(ch, w_mark, (void*)&flags[2]) == 1, "cancel a job in the ring");
    CHECK(dyn_pool_cancel(ch, w_mark, (void*)&flags[n - 3]) == 1, "cancel a job in the overflow");
    CHECK(dyn_pool_cancel(ch, w_mark, (void*)&flags[2]) == 0, "a second cancel of the same job is 0");
    CHECK(dyn_pool_cancel(ch, w_hold, NULL) == 0, "a running job cannot be cancelled");
    CHECK(dyn_pool_cancel(ch, w_mark, (void*)&g_ran) == 0, "an unknown job is 0");
    gate(1);
    CHECK(drain_until(ch, n + 1, 10000) == n + 1, "done runs for every job, cancelled ones included");
    CHECK(atomic_load(&flags[2]) == 0 && atomic_load(&flags[n - 3]) == 0, "cancelled work never ran");
    for (i = 0; i < n; i++)
        if (i != 2 && i != n - 3 && atomic_load(&flags[i]) != 1)
            break;
    CHECK(i == n, "every other job ran (first miss %d)", i);
    CHECK(dyn_pool_cancel(ch, w_mark, (void*)&flags[5]) == 0, "a finished job cannot be cancelled");
    dyn_pool_stats(p, &st);
    CHECK(st.cancelled == 2, "stats count 2 cancels (%llu)", st.cancelled);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  cancel (ring + overflow): ok\n");
}

#define T_POOL_BLOCK_NS 1000000LL
#define T_POOL_COOLDOWN_NS 2000000LL
static _Atomic long long g_last_end, g_stall_allow;

static long long t_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void d_slow(void* a)
{
    long long now, prev;
    (void)a;
    usleep(200);
    now = t_now_ns();
    prev = atomic_exchange(&g_last_end, now);
    if (prev && now - prev >= T_POOL_BLOCK_NS)
        atomic_fetch_add(&g_stall_allow, 1 + (now - prev) / T_POOL_COOLDOWN_NS);
}

static void case_tiny_io(void)
{
    dyn_pool_t* p = dyn_pool_new(1, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    int round, i, got = 0, n = 0;
    CHECK(p && ch, "pool + chan (tiny io)");
    if (!p || !ch)
        return;
    reset();
    atomic_store(&g_last_end, 0);
    atomic_store(&g_stall_allow, 0);
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (round = 0;; round++) {
            atomic_store(&g_last_end, 0);
            for (i = 0; i < 64; i++)
                n += dyn_pool_submit(ch, d_slow, d_count, NULL) == 0;
            got += drain_until(ch, 64, 5000);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 400)
                break;
        }
    }
    CHECK(got == n && n == (round + 1) * 64, "all %d tiny blocking-class jobs complete (%d)", n, got);
    dyn_pool_stats(p, &st);
    CHECK(st.blocking_spawned <= 2 + (unsigned long long)atomic_load(&g_stall_allow), "a busy but progressing blocking class spawns no elastic threads (spawned %llu, stall allowance %lld)", st.blocking_spawned, (long long)atomic_load(&g_stall_allow));
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  tiny blocking jobs: elastic spawns %llu, stall allowance %lld\n", st.blocking_spawned, (long long)atomic_load(&g_stall_allow));
}

static void w_spin50(void* arg)
{
    struct timespec t0, t1;
    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do
        clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) < 50000);
}

static long ms_since(const struct timespec* t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static void case_starve(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    _Atomic int hit = 0;
    struct timespec t0;
    long ms = 0;
    int i;
    CHECK(p && ch, "pool + chan (starve)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 2; i++)
        dyn_pool_submit(ch, w_hold, d_count, NULL);
    CHECK(wait_entered(2, 5000) == 2, "both threads hold a blocking job");
    for (i = 0; i < 16; i++)
        dyn_pool_submit_cpu(ch, w_spin50, d_count, NULL);
    dyn_pool_submit(ch, w_mark, d_count, (void*)&hit);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!atomic_load(&hit) && (ms = ms_since(&t0)) < 500) {
        while (dyn_pool_inflight(ch) < 20)
            dyn_pool_submit_cpu(ch, w_spin50, d_count, NULL);
        dyn_pool_drain(ch);
        usleep(100);
    }
    CHECK(atomic_load(&hit), "an io job runs within 500 ms while every thread blocks and cpu work streams (%ld ms)", ms);
    gate(1);
    for (i = 0; i < 2000 && dyn_pool_inflight(ch); i++) {
        dyn_pool_drain(ch);
        usleep(1000);
    }
    CHECK(dyn_pool_inflight(ch) == 0, "everything drains after release");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  starve: io probe ran after %ld ms under a cpu stream: ok\n", ms);
}

static void case_nopend(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    int i;
    CHECK(p && ch, "pool + chan (nopend)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 2; i++)
        dyn_pool_submit(ch, w_hold, d_count, NULL);
    CHECK(wait_entered(2, 5000) == 2, "both threads hold a blocking job");
    dyn_pool_submit_cpu(ch, w_spin50, d_count, NULL);
    usleep(30000);
    dyn_pool_stats(p, &st);
    CHECK(st.blocking_spawned == 0, "every thread blocked but nothing io queued: no elastic spawn (%llu)", st.blocking_spawned);
    gate(1);
    CHECK(drain_until(ch, 3, 5000) == 3, "all 3 jobs complete");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  nopend: no spawn without queued io: ok\n");
}

static void case_capio(void)
{
    dyn_pool_t* p = dyn_pool_new(8, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    _Atomic int hit = 0;
    struct timespec t0;
    long ms = 0;
    int i, e;
    CHECK(p && ch, "pool + chan (capio)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 10; i++)
        dyn_pool_submit_cpu(ch, w_hold, d_count, NULL);
    e = wait_entered(8, 3000);
    CHECK(e == 8, "cpu work runs pool-wide: 7 regular threads + the spare (%d)", e);
    usleep(50000);
    CHECK(atomic_load(&g_entered) == 8, "no 9th cpu job runs at once (%d)", atomic_load(&g_entered));
    dyn_pool_submit(ch, w_mark, d_count, (void*)&hit);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!atomic_load(&hit) && (ms = ms_since(&t0)) < 1000)
        usleep(200);
    CHECK(atomic_load(&hit), "an io job runs on the thread the cpu cap keeps free while cpu work holds (%ld ms)", ms);
    gate(1);
    CHECK(drain_until(ch, 11, 5000) == 11, "all 11 jobs complete");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  capio: cpu 8 wide, io served in %ld ms: ok\n", ms);
}

static void w_nop(void* arg)
{
    (void)arg;
}

static void case_capleak(void)
{
    dyn_pool_t* p = dyn_pool_new(8, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    _Atomic int hit = 0;
    struct timespec t0;
    long ms = 0;
    int i, k, got = 0, e;
    CHECK(p && ch, "pool + chan (capleak)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 40000; i += 4) {
        for (k = 0; k < 4; k++)
            dyn_pool_submit_cpu(ch, w_nop, d_count, NULL);
        got += drain_until(ch, i + 4 - got, 2000);
    }
    CHECK(got == 40000, "40000 racing cpu jobs complete (%d)", got);
    reset();
    for (i = 0; i < 10; i++)
        dyn_pool_submit_cpu(ch, w_hold, d_count, NULL);
    e = wait_entered(8, 3000);
    CHECK(e == 8, "after racing takes the cpu cap still admits 7 regular threads + the spare (%d)", e);
    dyn_pool_submit(ch, w_mark, d_count, (void*)&hit);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!atomic_load(&hit) && (ms = ms_since(&t0)) < 1000)
        usleep(200);
    CHECK(atomic_load(&hit), "after racing takes an io job still runs beside held cpu work (%ld ms)", ms);
    gate(1);
    CHECK(drain_until(ch, 11, 5000) == 11, "all 11 jobs complete (capleak)");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  capleak: 40000 racing takes, cap intact: ok\n");
}

static void case_smallcap(void)
{
    dyn_pool_t* p = dyn_pool_new(4, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    int i, e;
    CHECK(p && ch, "pool + chan (smallcap)");
    if (!p || !ch)
        return;
    reset();
    for (i = 0; i < 6; i++)
        dyn_pool_submit_cpu(ch, w_hold, d_count, NULL);
    e = wait_entered(4, 3000);
    CHECK(e == 4, "a pool under %u threads runs cpu work on all 4 regular threads (%d)", POOL_CPU_CAP_MIN, e);
    usleep(50000);
    dyn_pool_stats(p, &st);
    CHECK(atomic_load(&g_entered) == 4, "no 5th cpu job runs at once (%d)", atomic_load(&g_entered));
    CHECK(st.spare_kicks == 0, "no cpu cap under %u threads: the spare is never kicked (%llu)", POOL_CPU_CAP_MIN, st.spare_kicks);
    gate(1);
    CHECK(drain_until(ch, 6, 5000) == 6, "all 6 jobs complete (smallcap)");
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  smallcap: 4 threads, cpu 4 wide, no spare: ok\n");
}

static void case_cooldown(void)
{
    dyn_pool_t* p = dyn_pool_new(2, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    struct timespec t0;
    long us;
    int i, n = 2 + (int)POOL_ELASTIC_MAX + 4;
    CHECK(p && ch, "pool + chan (cooldown)");
    if (!p || !ch)
        return;
    reset();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++)
        dyn_pool_submit(ch, w_hold, d_count, NULL);
    CHECK(wait_entered(2 + (int)POOL_ELASTIC_MAX, 5000) == 2 + (int)POOL_ELASTIC_MAX, "the elastic tier fills to its cap");
    {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    }
    dyn_pool_stats(p, &st);
    CHECK(us * 1000L >= (long)((POOL_ELASTIC_MAX - 1) * POOL_SPAWN_COOLDOWN_NS * 9 / 10), "%u spawns are spaced by the cooldown: ramp %ld us >= %lld us", POOL_ELASTIC_MAX, us, (long long)((POOL_ELASTIC_MAX - 1) * POOL_SPAWN_COOLDOWN_NS * 9 / 10 / 1000));
    CHECK(st.blocking_spawned <= (unsigned long long)(us * 1000L / POOL_SPAWN_COOLDOWN_NS) + 1u, "spawned %llu <= ramp / cooldown + 1", st.blocking_spawned);
    gate(1);
    CHECK(drain_until(ch, n, 10000) == n, "all %d jobs complete", n);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
    printf("  cooldown: %u spawns in %ld us: ok\n", POOL_ELASTIC_MAX, us);
}

static void case_kicks(void)
{
    dyn_pool_t* p = dyn_pool_new(1, 64);
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    dyn_pool_stats_t st;
    int round, i, got = 0, n = 0;
    struct timespec t0, t1;
    CHECK(p && ch, "pool + chan (kicks)");
    if (!p || !ch)
        return;
    reset();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (round = 0;; round++) {
        for (i = 0; i < 8; i++)
            n += dyn_pool_submit(ch, NULL, d_count, NULL) == 0;
        got += drain_until(ch, 8, 5000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 200)
            break;
    }
    dyn_pool_stats(p, &st);
    CHECK(got == n, "all %d kick-probe jobs complete (%d)", n, got);
    {
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        CHECK(st.spare_kicks <= (unsigned long long)ms + 64u, "a saturated 1-thread pool kicks the spare at most once per ms (%llu kicks in %ld ms, %d rounds)", st.spare_kicks, ms, round + 1);
    }
    printf("  kicks: %d jobs, %d rounds, %llu spare kicks: ok\n", n, round + 1, st.spare_kicks);
    dyn_pool_chan_free(ch);
    dyn_pool_free(p);
}

static void case_fork(void)
{
    dyn_pool_t* p = dyn_pool_shared_acquire();
    dyn_pool_chan_t* ch = dyn_pool_chan_new(p);
    pid_t pid;
    int status = 0;
    CHECK(p && ch, "shared pool + chan (fork)");
    if (!p || !ch)
        return;
    pid = fork();
    if (pid == 0) {
        dyn_pool_t* q;
        dyn_pool_chan_t* qc;
        _Atomic int hit = 0;
        int bad = 0;
        errno = 0;
        if (dyn_pool_submit(ch, w_mark, d_count, (void*)&hit) == 0 || errno != ECHILD)
            bad |= 1;
        q = dyn_pool_shared_acquire();
        if (!q || q == p)
            bad |= 2;
        qc = q ? dyn_pool_chan_new(q) : NULL;
        if (!qc || dyn_pool_submit(qc, w_mark, d_count, (void*)&hit) != 0 || drain_until(qc, 1, 5000) != 1 || !hit)
            bad |= 4;
        _exit(bad);
    }
    CHECK(pid > 0, "fork");
    if (pid > 0) {
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "child: parent pool refuses with ECHILD (1), fresh shared pool (2), it runs jobs (4): bits %d",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    dyn_pool_chan_free(ch);
    dyn_pool_shared_release(p);
    printf("  fork: parent pool refused in the child, fresh pool works: ok\n");
}

int main(void)
{
    const char* only = getenv("POOL_POLICY_ONLY");
#define RUN(name, fn)                       \
    if (!only || strstr(only, name) != NULL) \
    fn()
    RUN("elastic", case_elastic_cap_retire);
    RUN("one", case_one_thread);
    RUN("overflow", case_overflow_fifo);
    RUN("hol", case_no_hol);
    RUN("cancel", case_cancel);
    RUN("tiny", case_tiny_io);
    RUN("kicks", case_kicks);
    RUN("starve", case_starve);
    RUN("nopend", case_nopend);
    RUN("capio", case_capio);
    RUN("capleak", case_capleak);
    RUN("smallcap", case_smallcap);
    RUN("cooldown", case_cooldown);
    RUN("fork", case_fork);
    if (fails == 0)
        printf("test_pool_policy: all tests passed\n");
    else
        printf("test_pool_policy: %d FAILED\n", fails);
    return fails != 0;
}
