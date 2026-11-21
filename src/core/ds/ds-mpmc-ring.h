#ifndef DS_MPMC_RING_H
#define DS_MPMC_RING_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DYN_DS_NO_THREADS
typedef struct dyn_ring dyn_ring_t;

dyn_ring_t* dyn_ring_new(size_t elem_size, size_t capacity);

void dyn_ring_free(dyn_ring_t* r);

size_t dyn_ring_capacity(const dyn_ring_t* r);

int dyn_ring_try_push(dyn_ring_t* r, const void* elem);

int dyn_ring_try_pop(dyn_ring_t* r, void* out);

size_t dyn_ring_size_approx(const dyn_ring_t* r);
#endif

#ifdef __cplusplus
}
#endif

#endif
