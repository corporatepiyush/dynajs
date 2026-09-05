/*
 * dyn-timer -- a min-heap of deadlines. Idle timeouts, connect and query
 * deadlines, and retry backoff all come from here.
 *
 * A binary min-heap, not the MinMaxHeap in dyn-ds.h: timers only ever pop the
 * minimum, and that adoption was measured 1.52-1.58x SLOWER for a pop_min-only
 * pattern (tests/bench_heap_adoption.c). Same verdict, same reason.
 *
 * Ordering is (deadline, seq) so timers armed for the same millisecond fire in
 * the order they were armed. Without the tiebreaker that order is arbitrary,
 * which is a flaky test waiting to happen.
 *
 * CANCELLATION IS O(log n), NOT LAZY. An idle timeout is re-armed on every
 * read, so a lazy "mark dead, skip on pop" scheme grows without bound on a
 * long-lived connection. Each timer keeps its heap index in a side table.
 *
 * NOT THREAD-SAFE: one wheel per event loop, touched only by that loop.
 */
#ifndef DYN_TIMER_H
#define DYN_TIMER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_timers dyn_timers_t;
typedef void (*dyn_timer_cb)(void *arg);

/* 0 is never a valid timer id, so a zeroed struct means "no timer armed". */
typedef uint32_t dyn_timer_id;
#define DYN_TIMER_NONE ((dyn_timer_id)0)

/* Monotonic milliseconds. Never walks backwards across a wall-clock change. */
uint64_t dyn_timer_now_ms(void);

dyn_timers_t *dyn_timers_new(void);
void dyn_timers_free(dyn_timers_t *t);

/* Arm `cb(arg)` for `now + delay_ms`. Returns an id, or DYN_TIMER_NONE on OOM.
 * `now` is passed in rather than read here so tests can drive time directly. */
dyn_timer_id dyn_timer_add(dyn_timers_t *t, uint64_t now, uint64_t delay_ms,
                           dyn_timer_cb cb, void *arg);

/* Disarm. Safe on DYN_TIMER_NONE, on an already-fired id, and on a stale id
 * from a previous generation -- all are no-ops returning 0. Returns 1 if a
 * live timer was actually removed. */
int dyn_timer_cancel(dyn_timers_t *t, dyn_timer_id id);

/* Milliseconds until the earliest deadline: 0 if one is already due, -1 if no
 * timer is armed. Capped at INT32_MAX so it can be handed to poll(). */
int dyn_timer_next_timeout(const dyn_timers_t *t, uint64_t now);

/* Fire every timer whose deadline has passed, earliest first. Returns how many
 * ran. A callback may arm or cancel timers, including its own id. */
int dyn_timer_run(dyn_timers_t *t, uint64_t now);

size_t dyn_timers_count(const dyn_timers_t *t);

#ifdef __cplusplus
}
#endif

#endif /* DYN_TIMER_H */
