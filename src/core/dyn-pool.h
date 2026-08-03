/*
 * dyn-pool -- a bounded work pool for operations that genuinely BLOCK.
 *
 * Not a general offload path. Handing work to another thread costs ~1-5us, so
 * it only pays when the work is longer than that: a page-cache-hit read is
 * ~200ns and belongs inline. Callers decide; this file just runs what it is
 * given. See NET_PLAN.md section 2.1 for the routing table.
 *
 * NO ALLOCATION PER OPERATION. Every job lives in a slot preallocated at
 * dyn_pool_new(); a full queue is refused, never grown.
 *
 * THREADING: `work` runs on a worker and must touch NOTHING owned by the
 * submitting thread except `arg`. `done` runs on the thread that calls
 * dyn_pool_drain(), so that is where a JSContext may be touched -- never in
 * `work`.
 *
 * BACKPRESSURE: dyn_pool_submit() NEVER blocks. A full queue returns -1, and
 * the caller is expected to do the work inline. Blocking the submitting thread
 * on a queue its own completions must drain is a deadlock.
 */
#ifndef DYN_POOL_H
#define DYN_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_pool dyn_pool_t;

/* A completion endpoint: its own ring and its own wake fd. ONE PER RUNTIME.
 * Worker threads are shared process-wide, but a completion must come back to
 * the runtime that submitted it -- with a single shared ring, whichever thread
 * drained first would run the other runtime's callbacks on the wrong context. */
typedef struct dyn_pool_chan dyn_pool_chan_t;

/* Runs on a worker thread (`work`) or on the draining thread (`done`). */
typedef void (*dyn_pool_fn)(void *arg);

/* max(number of CPUs, 4) unless overridden. Never 0. */
unsigned dyn_pool_default_threads(void);

/* Override that default (the --io-threads flag). Call ONCE, at startup, before
 * any pool exists: it is a plain global, not an atomic, and is safe only
 * because nothing else is running yet. `n` is clamped to 1..1024.
 *
 * n == 0 means INLINE-ONLY, not "auto": dyn_pool_new then returns NULL and
 * every submitter falls back to running the work on the calling thread. That
 * is the T1 topology, and it exists so T2 can be measured against it in the
 * SAME binary (NET_PLAN.md 2, 7). */
void dyn_pool_set_default_threads(unsigned n);

/* Whether the above put us in T1. For reporting which topology ran -- a
 * measurement that cannot say which arm it measured is not a measurement. */
int dyn_pool_is_inline_only(void);

/* `nthreads` 0 => dyn_pool_default_threads(). `queue_cap` 0 => 4x nthreads.
 * Threads are created eagerly, so a NULL return means the pool is unusable
 * rather than degraded. */
dyn_pool_t *dyn_pool_new(unsigned nthreads, unsigned queue_cap);

/* Joins every worker before freeing. Safe with jobs still queued: they are
 * dropped, and their `done` is NOT called. */
void dyn_pool_free(dyn_pool_t *p);

unsigned dyn_pool_threads(const dyn_pool_t *p);
unsigned dyn_pool_capacity(const dyn_pool_t *p);

/* Open a completion endpoint on `p`. NULL on failure. */
dyn_pool_chan_t *dyn_pool_chan_new(dyn_pool_t *p);

/* Close it. Undrained completions are DISCARDED and their `done` is not run --
 * a runtime that is going away must not have callbacks fired into it. Blocks
 * until any of this channel's jobs still running on a worker have finished, so
 * no worker can touch the freed channel afterwards. */
void dyn_pool_chan_free(dyn_pool_chan_t *ch);

/* Pollable fd, readable when this channel has a completion pending. Fold into
 * that runtime's event loop; drain with dyn_pool_drain(). Never read it
 * directly -- drain consumes the byte. */
int dyn_pool_wake_fd(const dyn_pool_chan_t *ch);

/* Queue `work`, then `done` on the thread that drains `ch`. Returns 0, or -1
 * if the pool is full or stopping -- see BACKPRESSURE above. */
int dyn_pool_submit(dyn_pool_chan_t *ch, dyn_pool_fn work, dyn_pool_fn done,
                    void *arg);

/* Run this channel's pending `done` callbacks on the calling thread. Returns
 * how many ran. */
int dyn_pool_drain(dyn_pool_chan_t *ch);

/* Jobs submitted on `ch` whose `done` has not yet run. For loop-liveness. */
size_t dyn_pool_inflight(const dyn_pool_chan_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* DYN_POOL_H */
