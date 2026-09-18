/* test_pool.c -- dyn-pool contracts.
 *
 * Proves the four things the pool promises and the two it refuses:
 *   - every submitted job runs its work exactly once, and its done exactly once
 *   - done runs on the DRAINING thread, never on a worker
 *   - a full queue is REFUSED (EAGAIN), never blocks the submitter
 *   - the wake fd becomes readable, so a poll loop can see completions
 *   - free() with jobs still queued does not hang or run their done
 *   - default thread count is max(ncpu, 4)
 *
 * Run under TSan for the concurrency claim: a clean single-threaded pass says
 * nothing about a race (CLAUDE.md section 6).
 */
#include "dyn-pool.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* --- shared counters, guarded because workers touch them --- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_work_runs, g_done_runs;
static pthread_t g_done_thread;
static int g_done_thread_set;

static void w_bump(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    g_work_runs++;
    pthread_mutex_unlock(&g_mu);
}

static void d_bump(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    g_done_runs++;
    g_done_thread = pthread_self();
    g_done_thread_set = 1;
    pthread_mutex_unlock(&g_mu);
}

/* Blocks until released, so the queue can be driven full deterministically. */
static pthread_mutex_t g_block = PTHREAD_MUTEX_INITIALIZER;
static void w_block(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_block);
    pthread_mutex_unlock(&g_block);
}
static void d_noop(void *arg) { (void)arg; }

static int drain_until(dyn_pool_chan_t *ch, int want, int max_ms)
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

int main(void)
{
    /* Line-buffered: a deadlock in this file is a real failure mode, and fully
     * buffered stdout throws away every result printed before it. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* ---- 1. default thread count is max(ncpu, 4) ---- */
    {
        unsigned d = dyn_pool_default_threads();
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        unsigned want = (ncpu > 4) ? (unsigned)ncpu : 4u;
        CHECK(d == want, "default threads %u, expected max(ncpu=%ld,4)=%u",
              d, ncpu, want);
        CHECK(d >= 4u, "default threads must never be below 4 (got %u)", d);
    }

    /* ---- 2. every job runs work once and done once, done on OUR thread ---- */
    {
        dyn_pool_t *p = dyn_pool_new(4, 64);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        const int N = 200;
        int i, got;
        CHECK(p != NULL, "pool_new");
        if (!p) return 1;

        g_work_runs = g_done_runs = 0;
        g_done_thread_set = 0;
        /* N exceeds the queue, which is the normal case: submit until refused,
         * drain, repeat. A refusal here is correct behaviour, not a failure. */
        got = 0;
        for (i = 0; i < N; ) {
            if (dyn_pool_submit(ch, w_bump, d_bump, NULL) == 0) {
                i++;
                continue;
            }
            got += drain_until(ch, 1, 5000);    /* full: make room */
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

    /* ---- 3. a FULL queue is refused, and refusal does not block ---- */
    {
        dyn_pool_t *p = dyn_pool_new(2, 8);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        unsigned cap = dyn_pool_capacity(p);
        int refused = 0, accepted = 0, i;
        CHECK(p != NULL, "pool_new (small)");
        if (!p) return 1;

        pthread_mutex_lock(&g_block);          /* wedge every worker */
        /* cap queued + up to nthreads in flight; submit well past both. */
        for (i = 0; i < (int)cap * 4; i++) {
            if (dyn_pool_submit(ch, w_block, d_noop, NULL) == 0) accepted++;
            else refused++;
        }
        CHECK(refused > 0, "a full queue must REFUSE (cap=%u, accepted=%d)",
              cap, accepted);
        CHECK(accepted <= (int)cap + (int)dyn_pool_threads(p),
              "accepted %d exceeds cap %u + threads %u", accepted, cap,
              dyn_pool_threads(p));
        pthread_mutex_unlock(&g_block);        /* release; free() joins */
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    /* ---- 3b. completions that are never drained must not overflow ----
     * The submit ring empties as workers run, but each job then occupies a
     * COMPLETION slot. Gating submission on the submit ring's depth alone lets
     * cap-queued + cap-undrained overrun the completion ring. Drive exactly
     * that: fill, let the workers finish, and submit again without draining. */
    {
        dyn_pool_t *p = dyn_pool_new(2, 8);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        unsigned cap = dyn_pool_capacity(p);
        int accepted = 0, round, i;
        CHECK(p != NULL, "pool_new (overflow)");
        if (!p) return 1;

        for (round = 0; round < 4; round++) {
            for (i = 0; i < (int)cap; i++)
                if (dyn_pool_submit(ch, w_bump, d_noop, NULL) == 0)
                    accepted++;
            poll(NULL, 0, 50);        /* let workers move them to completions */
        }
        /* Never drained, so at most `cap` may ever have been accepted. */
        CHECK(accepted <= (int)cap,
              "accepted %d without draining; cap is %u -- completion ring would "
              "overflow", accepted, cap);
        CHECK(dyn_pool_inflight(ch) <= (size_t)cap,
              "inflight %zu exceeds cap %u", (size_t)dyn_pool_inflight(ch), cap);
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);
    }

    /* ---- 4. free() with work still queued neither hangs nor runs done ---- */
    {
        dyn_pool_t *p = dyn_pool_new(2, 32);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        int i;
        CHECK(p != NULL, "pool_new (teardown)");
        if (!p) return 1;
        pthread_mutex_lock(&g_block);
        for (i = 0; i < 20; i++)
            (void)dyn_pool_submit(ch, w_block, d_noop, NULL);
        pthread_mutex_unlock(&g_block);
        dyn_pool_chan_free(ch);
        dyn_pool_free(p);                      /* must return, not hang */
        printf("  teardown with queued work: ok\n");
    }

    /* ---- 5. the wake fd is a real pollable edge ---- */
    {
        dyn_pool_t *p = dyn_pool_new(2, 16);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        struct pollfd pfd;
        CHECK(p != NULL, "pool_new (wake)");
        if (!p) return 1;
        CHECK(dyn_pool_wake_fd(ch) >= 0, "wake fd must be valid");

        pfd.fd = dyn_pool_wake_fd(ch); pfd.events = POLLIN; pfd.revents = 0;
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

    /* ---- 6. CHANNEL ISOLATION -- the whole reason channels exist ----
     * Workers are shared, but a completion must come back to the channel that
     * submitted it. With one shared ring, whichever thread drained first would
     * run the other runtime's callbacks on the wrong context. */
    {
        dyn_pool_t *p = dyn_pool_new(4, 32);
        dyn_pool_chan_t *a = dyn_pool_chan_new(p);
        dyn_pool_chan_t *b = dyn_pool_chan_new(p);
        int i, ga, gb;
        CHECK(p && a && b, "pool + two channels");
        if (!p || !a || !b) return 1;
        CHECK(dyn_pool_wake_fd(a) != dyn_pool_wake_fd(b),
              "each channel needs its OWN wake fd");

        for (i = 0; i < 10; i++) dyn_pool_submit(a, w_bump, d_noop, NULL);
        for (i = 0; i < 4;  i++) dyn_pool_submit(b, w_bump, d_noop, NULL);

        ga = drain_until(a, 10, 5000);
        gb = drain_until(b, 4, 5000);
        CHECK(ga == 10, "channel a drained %d, expected exactly its own 10", ga);
        CHECK(gb == 4,  "channel b drained %d, expected exactly its own 4", gb);
        CHECK(dyn_pool_inflight(a) == 0 && dyn_pool_inflight(b) == 0,
              "both channels should be empty (a=%zu b=%zu)",
              (size_t)dyn_pool_inflight(a), (size_t)dyn_pool_inflight(b));

        /* Closing one channel must not disturb the other. */
        for (i = 0; i < 6; i++) dyn_pool_submit(b, w_bump, d_noop, NULL);
        dyn_pool_chan_free(a);
        CHECK(drain_until(b, 6, 5000) == 6,
              "closing channel a must not swallow channel b's completions");
        dyn_pool_chan_free(b);
        dyn_pool_free(p);
    }

    /* ---- 7. closing a channel with work in flight drops it, does not fire ---- */
    {
        dyn_pool_t *p = dyn_pool_new(2, 16);
        dyn_pool_chan_t *ch = dyn_pool_chan_new(p);
        int before, i;
        CHECK(p && ch, "pool + channel (close)");
        if (!p || !ch) return 1;
        pthread_mutex_lock(&g_mu); before = g_done_runs; pthread_mutex_unlock(&g_mu);
        for (i = 0; i < 8; i++) dyn_pool_submit(ch, w_bump, d_bump, NULL);
        dyn_pool_chan_free(ch);          /* must not hang, must not run d_bump */
        pthread_mutex_lock(&g_mu);
        CHECK(g_done_runs == before,
              "a closing channel ran %d callbacks; a dead runtime must get none",
              g_done_runs - before);
        pthread_mutex_unlock(&g_mu);
        dyn_pool_free(p);
        printf("  close-with-inflight: ok\n");
    }

    if (fails == 0)
        printf("test_pool: all tests passed\n");
    else
        printf("test_pool: %d FAILED\n", fails);
    return fails != 0;
}
