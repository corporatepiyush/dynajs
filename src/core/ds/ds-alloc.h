#ifndef DS_ALLOC_H
#define DS_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

void dyn_ds_set_allocator(void* (*malloc_fn)(size_t), void* (*realloc_fn)(void*, size_t), void (*free_fn)(void*));

void* dyn_ds_malloc(size_t n);

void* dyn_ds_calloc(size_t n, size_t size);

void* dyn_ds_realloc(void* p, size_t n);

void dyn_ds_free(void* p);

#ifdef __cplusplus
}
#endif

#endif
