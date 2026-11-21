#ifndef DYN_POOL_H
#define DYN_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_pool dyn_pool_t;

typedef struct dyn_pool_chan dyn_pool_chan_t;

typedef void (*dyn_pool_fn)(void* arg);

typedef void (*dyn_pool_notify_fn)(void* ctx);

unsigned dyn_pool_max_threads(void);

unsigned dyn_pool_default_threads(void);

unsigned dyn_pool_thread_override(void);

void dyn_pool_set_default_threads(unsigned n);

int dyn_pool_is_inline_only(void);

dyn_pool_t* dyn_pool_new(unsigned nthreads, unsigned queue_cap);

void dyn_pool_free(dyn_pool_t* p);

dyn_pool_t* dyn_pool_shared_acquire(void);

void dyn_pool_shared_release(dyn_pool_t* p);

unsigned dyn_pool_shared_refs(void);

unsigned dyn_pool_threads(const dyn_pool_t* p);
unsigned dyn_pool_capacity(const dyn_pool_t* p);

typedef struct {
    unsigned long long parks;
    unsigned threads;
    unsigned blocking_live;
    unsigned blocking_peak;
    unsigned long long blocking_spawned;
    unsigned long long blocking_retired;
    unsigned long long overflowed;
    unsigned long long overflow_peak;
    unsigned long long cancelled;
    unsigned long long spare_kicks;
    unsigned long long spare_parks;
    unsigned io_running;
    unsigned cpu_running;
} dyn_pool_stats_t;

void dyn_pool_stats(const dyn_pool_t* p, dyn_pool_stats_t* st);

dyn_pool_chan_t* dyn_pool_chan_new(dyn_pool_t* p);

dyn_pool_chan_t* dyn_pool_chan_new_notify(dyn_pool_t* p, dyn_pool_notify_fn fn, void* ctx);

void dyn_pool_chan_free(dyn_pool_chan_t* ch);

int dyn_pool_wake_fd(const dyn_pool_chan_t* ch);

int dyn_pool_submit(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg);

int dyn_pool_submit_cpu(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg);

int dyn_pool_cpu_room(const dyn_pool_chan_t* ch);

int dyn_pool_cancel(dyn_pool_chan_t* ch, dyn_pool_fn work, void* arg);

int dyn_pool_drain(dyn_pool_chan_t* ch);

size_t dyn_pool_inflight(const dyn_pool_chan_t* ch);

#ifdef __cplusplus
}
#endif

#endif
