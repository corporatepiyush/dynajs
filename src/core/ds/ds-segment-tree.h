#ifndef DS_SEGMENT_TREE_H
#define DS_SEGMENT_TREE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double* tree;
    uint32_t n;
    int op;
} dyn_segtree_t;

double dyn_segtree_identity(int op);

int dyn_segtree_init(dyn_segtree_t* s, uint32_t n, int op);

void dyn_segtree_free(dyn_segtree_t* s);

void dyn_segtree_set(dyn_segtree_t* s, uint32_t i, double v);

double dyn_segtree_query(const dyn_segtree_t* s, uint32_t lo, uint32_t hi);

#ifdef __cplusplus
}
#endif

#endif
