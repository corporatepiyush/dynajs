/* test_timer.c -- dyn-timer contracts.
 *
 * Time is INJECTED, never read from the clock: a test that asserts durations
 * fails for reasons that are not bugs (CLAUDE.md section 6). Every case below
 * drives `now` by hand and asserts ORDER and COUNT.
 */
#include "dyn-timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static char order[256];
static size_t order_n;
static void rec(void *arg) { if (order_n < sizeof(order) - 1) order[order_n++] = (char)(intptr_t)arg; }

static dyn_timers_t *T;
static dyn_timer_id self_id;
static int rearm_count;
static uint64_t g_now;          /* the test drives this; the callback reads it */
static void rearm(void *arg)
{
    rec(arg);
    if (rearm_count++ < 2)
        dyn_timer_add(T, g_now, 10, rearm, (void *)(intptr_t)'R');
}
static void cancel_other(void *arg)
{
    rec(arg);
    dyn_timer_cancel(T, self_id);      /* cancel a sibling from inside a callback */
}

int main(void)
{
    /* ---- 1. fire in deadline order ---- */
    {
        dyn_timers_t *t = dyn_timers_new();
        order_n = 0; memset(order, 0, sizeof(order));
        dyn_timer_add(t, 0, 30, rec, (void *)(intptr_t)'c');
        dyn_timer_add(t, 0, 10, rec, (void *)(intptr_t)'a');
        dyn_timer_add(t, 0, 20, rec, (void *)(intptr_t)'b');
        CHECK(dyn_timers_count(t) == 3, "count %zu, want 3", dyn_timers_count(t));
        CHECK(dyn_timer_run(t, 5) == 0, "nothing is due at t=5");
        CHECK(dyn_timer_run(t, 25) == 2, "two due at t=25");
        CHECK(dyn_timer_run(t, 100) == 1, "one due at t=100");
        CHECK(strcmp(order, "abc") == 0, "order '%s', want 'abc'", order);
        CHECK(dyn_timers_count(t) == 0, "heap should be empty");
        dyn_timers_free(t);
    }

    /* ---- 2. equal deadlines fire FIFO (the start_id tiebreaker) ---- */
    {
        dyn_timers_t *t = dyn_timers_new();
        int i;
        order_n = 0; memset(order, 0, sizeof(order));
        for (i = 0; i < 8; i++)
            dyn_timer_add(t, 0, 50, rec, (void *)(intptr_t)('0' + i));
        CHECK(dyn_timer_run(t, 50) == 8, "all 8 due");
        CHECK(strcmp(order, "01234567") == 0,
              "equal deadlines must fire FIFO, got '%s'", order);
        dyn_timers_free(t);
    }

    /* ---- 3. cancel actually removes, and is O(1)-ish in garbage terms ---- */
    {
        dyn_timers_t *t = dyn_timers_new();
        dyn_timer_id a, b, c;
        order_n = 0; memset(order, 0, sizeof(order));
        a = dyn_timer_add(t, 0, 10, rec, (void *)(intptr_t)'a');
        b = dyn_timer_add(t, 0, 20, rec, (void *)(intptr_t)'b');
        c = dyn_timer_add(t, 0, 30, rec, (void *)(intptr_t)'c');
        CHECK(dyn_timer_cancel(t, b) == 1, "cancel(b) should report a removal");
        CHECK(dyn_timers_count(t) == 2, "count %zu after cancel, want 2",
              dyn_timers_count(t));
        CHECK(dyn_timer_cancel(t, b) == 0, "double cancel must be a no-op");
        CHECK(dyn_timer_cancel(t, DYN_TIMER_NONE) == 0, "cancel(NONE) no-op");
        CHECK(dyn_timer_run(t, 100) == 2, "two survivors fire");
        CHECK(strcmp(order, "ac") == 0, "order '%s', want 'ac'", order);
        (void)a; (void)c;
        dyn_timers_free(t);
    }

    /* ---- 4. re-arm churn does not grow the heap (the idle-timeout pattern) ----
     * A lazy-cancel implementation leaks one dead entry per re-arm; this is the
     * case that catches it. */
    {
        dyn_timers_t *t = dyn_timers_new();
        dyn_timer_id id = DYN_TIMER_NONE;
        int i;
        for (i = 0; i < 10000; i++) {
            dyn_timer_cancel(t, id);
            id = dyn_timer_add(t, (uint64_t)i, 1000, rec, (void *)(intptr_t)'x');
            CHECK(dyn_timers_count(t) <= 1 || i == 0,
                  "re-arm %d left %zu timers; cancellation is leaking",
                  i, dyn_timers_count(t));
            if (dyn_timers_count(t) > 1) break;
        }
        CHECK(dyn_timers_count(t) == 1, "exactly one armed after churn, got %zu",
              dyn_timers_count(t));
        dyn_timers_free(t);
    }

    /* ---- 5. next_timeout drives a poll() correctly ---- */
    {
        dyn_timers_t *t = dyn_timers_new();
        CHECK(dyn_timer_next_timeout(t, 0) == -1, "no timers => -1 (block)");
        dyn_timer_add(t, 100, 50, rec, (void *)(intptr_t)'z');
        CHECK(dyn_timer_next_timeout(t, 100) == 50, "50ms out");
        CHECK(dyn_timer_next_timeout(t, 140) == 10, "10ms out");
        CHECK(dyn_timer_next_timeout(t, 150) == 0, "due now => 0, never negative");
        CHECK(dyn_timer_next_timeout(t, 999999) == 0, "overdue => 0, never negative");
        dyn_timers_free(t);
    }

    /* ---- 6. a callback may arm and cancel from inside run() ---- */
    {
        order_n = 0; memset(order, 0, sizeof(order));
        T = dyn_timers_new();
        rearm_count = 0;
        dyn_timer_add(T, 100, 0, rearm, (void *)(intptr_t)'R');
        g_now = 100; CHECK(dyn_timer_run(T, 100) == 1, "the re-armed one is NOT due yet");
        g_now = 110; CHECK(dyn_timer_run(T, 110) == 1, "re-armed fires at +10");
        g_now = 120; CHECK(dyn_timer_run(T, 120) == 1, "and again");
        CHECK(rearm_count == 3, "rearm ran %d times, want 3", rearm_count);
        dyn_timers_free(T);

        order_n = 0; memset(order, 0, sizeof(order));
        T = dyn_timers_new();
        self_id = DYN_TIMER_NONE;
        dyn_timer_add(T, 0, 10, cancel_other, (void *)(intptr_t)'k');
        self_id = dyn_timer_add(T, 0, 20, rec, (void *)(intptr_t)'v');
        CHECK(dyn_timer_run(T, 100) == 1,
              "the victim was cancelled from inside the callback");
        CHECK(strcmp(order, "k") == 0, "order '%s', want 'k'", order);
        dyn_timers_free(T);
    }

    /* ---- 7. a stale id from a reused slot must not cancel the new owner ---- */
    {
        dyn_timers_t *t = dyn_timers_new();
        dyn_timer_id old, fresh;
        old = dyn_timer_add(t, 0, 10, rec, (void *)(intptr_t)'o');
        dyn_timer_cancel(t, old);                 /* frees the slot */
        fresh = dyn_timer_add(t, 0, 10, rec, (void *)(intptr_t)'n');
        CHECK(fresh != old, "a reused slot must mint a NEW id (gen bump)");
        CHECK(dyn_timer_cancel(t, old) == 0,
              "a stale id must NOT cancel the timer that reused its slot");
        CHECK(dyn_timers_count(t) == 1, "the fresh timer must still be armed");
        dyn_timers_free(t);
    }

    /* ---- 8. monotonic clock does not go backwards ---- */
    {
        uint64_t a = dyn_timer_now_ms(), b, i;
        for (i = 0; i < 200000; i++) { b = dyn_timer_now_ms(); CHECK(b >= a, "clock went backwards"); a = b; }
    }

    if (fails == 0) printf("test_timer: all tests passed\n");
    else printf("test_timer: %d FAILED\n", fails);
    return fails != 0;
}
