#ifndef DS_XARRAY_H
#define DS_XARRAY_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_xarray dyn_xarray_t;

dyn_xarray_t* dyn_xarray_new(const dyn_ds_ops* val);

void dyn_xarray_free(dyn_xarray_t* x);

size_t dyn_xarray_count(const dyn_xarray_t* x);

void* dyn_xarray_load(const dyn_xarray_t* x, uint64_t index);

int dyn_xarray_store(dyn_xarray_t* x, uint64_t index, const void* val);

int dyn_xarray_erase(dyn_xarray_t* x, uint64_t index);

int dyn_xarray_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, void** val);

int dyn_xarray_set_mark(dyn_xarray_t* x, uint64_t index, unsigned mark, int on);

int dyn_xarray_get_mark(const dyn_xarray_t* x, uint64_t index, unsigned mark);

int dyn_xarray_find_marked(const dyn_xarray_t* x, uint64_t* index, uint64_t max, unsigned mark, void** val);

#ifdef __cplusplus
}
#endif

#endif
