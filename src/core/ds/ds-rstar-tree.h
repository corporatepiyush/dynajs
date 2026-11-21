#ifndef DS_RSTAR_TREE_H
#define DS_RSTAR_TREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_RSTAR_MAX_DIMS 8

typedef struct dyn_rstar dyn_rstar_t;

typedef int (*dyn_rstar_visit)(const double* lo, const double* hi, uint64_t id, void* ud);

dyn_rstar_t* dyn_rstar_new(unsigned dims);

void dyn_rstar_free(dyn_rstar_t* t);

size_t dyn_rstar_size(const dyn_rstar_t* t);

unsigned dyn_rstar_height(const dyn_rstar_t* t);

int dyn_rstar_insert(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id);

int dyn_rstar_remove(dyn_rstar_t* t, const double* lo, const double* hi, uint64_t id);

size_t dyn_rstar_search(const dyn_rstar_t* t, const double* lo, const double* hi, dyn_rstar_visit fn, void* ud);

size_t dyn_rstar_nearest(const dyn_rstar_t* t, const double* point, size_t k, uint64_t* ids, double* dist2);

int dyn_rstar_check(const dyn_rstar_t* t);

int dyn_rstar_bulk_load(dyn_rstar_t* t, const double* boxes, const uint64_t* ids, size_t n);

#ifdef __cplusplus
}
#endif

#endif
