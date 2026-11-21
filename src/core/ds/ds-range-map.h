#ifndef DS_RANGE_MAP_H
#define DS_RANGE_MAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_rmap dyn_rmap_t;

dyn_rmap_t* dyn_rmap_new(void);

void dyn_rmap_free(dyn_rmap_t* m, dyn_cell_free_fn fn, void* ud);

int dyn_rmap_put(dyn_rmap_t* m, double lo, double hi, const dyn_cell_t* v,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud);

const dyn_cell_t* dyn_rmap_get(const dyn_rmap_t* m, double x);

int dyn_rmap_remove(dyn_rmap_t* m, double lo, double hi,
    dyn_cell_dup_fn dup, dyn_cell_free_fn fn, void* ud);

uint32_t dyn_rmap_count(const dyn_rmap_t* m);

int dyn_rmap_at(const dyn_rmap_t* m, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v);

#ifdef __cplusplus
}
#endif

#endif
