#ifndef DS_CBTREE_H
#define DS_CBTREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_cbt dyn_cbt_t;

typedef int (*dyn_cbt_visit_fn)(const void* key, void* val, void* ud);

dyn_cbt_t* dyn_cbt_new(const dyn_ds_ops* key, const dyn_ds_ops* val);

void dyn_cbt_free(dyn_cbt_t* t);

size_t dyn_cbt_size(const dyn_cbt_t* t);

void* dyn_cbt_get(const dyn_cbt_t* t, const void* key);

int dyn_cbt_put(dyn_cbt_t* t, const void* key, const void* val);

int dyn_cbt_remove(dyn_cbt_t* t, const void* key);

size_t dyn_cbt_rank(const dyn_cbt_t* t, const void* key);

int dyn_cbt_at(const dyn_cbt_t* t, size_t i, const void** key, void** val);

const void* dyn_cbt_bound(const dyn_cbt_t* t, const void* key, int floor);

int dyn_cbt_walk(const dyn_cbt_t* t, dyn_cbt_visit_fn fn, void* ud);

size_t dyn_cbt_bytes(const dyn_cbt_t* t);

int dyn_cbt_check(const dyn_cbt_t* t);

#ifdef __cplusplus
}
#endif

#endif
