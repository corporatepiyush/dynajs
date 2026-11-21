#ifndef DS_INTERVAL_TREE_H
#define DS_INTERVAL_TREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_itree dyn_itree_t;

dyn_itree_t* dyn_itree_new(void);

void dyn_itree_free(dyn_itree_t* t, dyn_cell_free_fn fn, void* ud);

int dyn_itree_insert(dyn_itree_t* t, double lo, double hi,
    const dyn_cell_t* v);

uint32_t dyn_itree_size(const dyn_itree_t* t);

uint32_t dyn_itree_query(dyn_itree_t* t, double lo, double hi,
    uint32_t* out, uint32_t cap);

int dyn_itree_at(const dyn_itree_t* t, uint32_t i, double* lo, double* hi,
    const dyn_cell_t** v);

int dyn_itree_remove_at(dyn_itree_t* t, uint32_t i, dyn_cell_t* out);

#ifdef __cplusplus
}
#endif

#endif
