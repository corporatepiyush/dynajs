#ifndef DS_BTREE_H
#define DS_BTREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_btree dyn_btree_t;

#define DYN_BTREE_ORDER 32

dyn_btree_t* dyn_btree_new(void);

void dyn_btree_free(dyn_btree_t* t, dyn_cell_free_fn fn, void* ud);

uint32_t dyn_btree_size(const dyn_btree_t* t);

int dyn_btree_set(dyn_btree_t* t, double k, const dyn_cell_t* v,
    dyn_cell_t* old, int* replaced);

const dyn_cell_t* dyn_btree_get(const dyn_btree_t* t, double k);

int dyn_btree_del(dyn_btree_t* t, double k, dyn_cell_t* out);

int dyn_btree_ceil(const dyn_btree_t* t, double k, double* out);

int dyn_btree_floor(const dyn_btree_t* t, double k, double* out);

int dyn_btree_first(const dyn_btree_t* t, double* out);

int dyn_btree_last(const dyn_btree_t* t, double* out);

uint32_t dyn_btree_get_keys(const dyn_btree_t* t, double* out);

typedef int (*dyn_btree_leaf_fn)(void* ud, const double* keys,
    const dyn_cell_t* vals, uint32_t n);

uint32_t dyn_btree_walk_leaves(const dyn_btree_t* t, dyn_btree_leaf_fn fn,
    void* ud);

typedef struct {
    void* leaf;
    uint32_t i;
} dyn_btree_iter;

int dyn_btree_iter_begin(const dyn_btree_t* t, dyn_btree_iter* it);

int dyn_btree_iter_seek(const dyn_btree_t* t, double k, dyn_btree_iter* it);

int dyn_btree_iter_get(const dyn_btree_iter* it, double* k,
    const dyn_cell_t** v);

int dyn_btree_iter_next(dyn_btree_iter* it);

#ifdef __cplusplus
}
#endif

#endif
