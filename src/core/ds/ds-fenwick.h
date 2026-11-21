#ifndef DS_FENWICK_H
#define DS_FENWICK_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double* tree;
    uint32_t n;
} dyn_fenwick_t;

int dyn_fenwick_init(dyn_fenwick_t* f, uint32_t n);

void dyn_fenwick_free(dyn_fenwick_t* f);

double dyn_fenwick_range(const dyn_fenwick_t* f, uint32_t lo, uint32_t hi);

static inline void dyn_fenwick_add(dyn_fenwick_t* f, uint32_t i, double delta)
{
    uint32_t idx;
    for (idx = i + 1; idx <= f->n; idx += idx & (~idx + 1))
        f->tree[idx] += delta;
}

static inline double dyn_fenwick_prefix(const dyn_fenwick_t* f, uint32_t i)
{
    uint32_t idx;
    double s = 0.0;
    for (idx = i + 1; idx > 0; idx -= idx & (~idx + 1))
        s += f->tree[idx];
    return s;
}

#ifdef __cplusplus
}
#endif

#endif
