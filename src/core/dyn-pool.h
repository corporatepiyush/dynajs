#ifndef DYN_POOL_H
#define DYN_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_pool dyn_pool_t;

typedef struct dyn_pool_chan dyn_pool_chan_t;

typedef void (*dyn_pool_fn)(void* arg);

unsigned dyn_pool_default_threads(void);

void dyn_pool_set_default_threads(unsigned n);

int dyn_pool_is_inline_only(void);

dyn_pool_t* dyn_pool_new(unsigned nthreads, unsigned queue_cap);

void dyn_pool_free(dyn_pool_t* p);

unsigned dyn_pool_threads(const dyn_pool_t* p);
unsigned dyn_pool_capacity(const dyn_pool_t* p);

dyn_pool_chan_t* dyn_pool_chan_new(dyn_pool_t* p);

void dyn_pool_chan_free(dyn_pool_chan_t* ch);

int dyn_pool_wake_fd(const dyn_pool_chan_t* ch);

int dyn_pool_submit(dyn_pool_chan_t* ch, dyn_pool_fn work, dyn_pool_fn done,
    void* arg);

int dyn_pool_drain(dyn_pool_chan_t* ch);

size_t dyn_pool_inflight(const dyn_pool_chan_t* ch);

#ifdef __cplusplus
}
#endif

#endif
