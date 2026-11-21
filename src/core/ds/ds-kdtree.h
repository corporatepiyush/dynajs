#ifndef DS_KDTREE_H
#define DS_KDTREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_KDTREE_LEAF 12
#define DYN_KDTREE_MAX_DIMS 64

typedef struct {
    uint32_t lo, hi;
    int32_t l, r;
    double split;
    uint32_t dim;
} dyn_kdtree_node;

typedef struct {
    uint32_t d, n;
    double* pts;
    uint32_t* perm;
    dyn_kdtree_node* nodes;
    uint32_t nn, ncap;
    uint64_t rng;
} dyn_kdtree_t;

typedef struct {
    double d2;
    uint32_t i;
} dyn_kdtree_hit;

typedef struct {
    const dyn_kdtree_t* t;
    const double* q;
    dyn_kdtree_hit* h;
    uint32_t k, n;
} dyn_kdtree_knn_ctx;

typedef struct {
    dyn_kdtree_hit* h;
    uint32_t n, cap;
    int oom;
} dyn_kdtree_list;

void dyn_kdtree_free(dyn_kdtree_t* t);

int dyn_kdtree_init(dyn_kdtree_t* t, double* pts, uint32_t n, uint32_t d);

void dyn_kdtree_knn(dyn_kdtree_knn_ctx* c, int32_t ni);

int dyn_kdtree_hit_cmp(const void* a, const void* b);

void dyn_kdtree_radius(const dyn_kdtree_t* t, int32_t ni, const double* q, double r2, dyn_kdtree_list* out);

void dyn_kdtree_box(const dyn_kdtree_t* t, int32_t ni, const double* lo, const double* hi, dyn_kdtree_list* out);

#ifdef __cplusplus
}
#endif

#endif
