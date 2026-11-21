#ifndef DS_HEAP_H
#define DS_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_HEAP_ABORT (-2147483647 - 1)

typedef int (*dyn_heap_cmp_fn)(void* ud, const dyn_cell_t* a, const dyn_cell_t* b);

typedef struct {
    dyn_cell_t* items;
    uint32_t count;
    uint32_t cap;
} dyn_heap_t;

void dyn_heap_init(dyn_heap_t* h);

void dyn_heap_free(dyn_heap_t* h, dyn_cell_free_fn release, void* ud);

int dyn_heap_reserve(dyn_heap_t* h, uint32_t need);

static inline int dyn_heap_sift_up(dyn_heap_t* h, uint32_t idx, dyn_heap_cmp_fn cmp, void* ud)
{
    while (idx > 0) {
        uint32_t parent = (idx - 1) / 2;
        int c = cmp(ud, &h->items[idx], &h->items[parent]);
        dyn_cell_t tmp;
        if (c == DYN_HEAP_ABORT)
            return -1;
        if (c >= 0)
            break;
        tmp = h->items[idx];
        h->items[idx] = h->items[parent];
        h->items[parent] = tmp;
        idx = parent;
    }
    return 0;
}

static inline int dyn_heap_sift_down(dyn_heap_t* h, uint32_t idx, dyn_heap_cmp_fn cmp, void* ud)
{
    uint32_t path[64];
    int depth = 0, m, c;
    dyn_cell_t x = h->items[idx];
    path[0] = idx;
    while (2 * path[depth] + 1 < h->count && depth < 63) {
        uint32_t l = 2 * path[depth] + 1, r = l + 1, pick = l;
        if (r < h->count) {
            c = cmp(ud, &h->items[r], &h->items[l]);
            if (c == DYN_HEAP_ABORT)
                return -1;
            if (c < 0)
                pick = r;
        }
        path[++depth] = pick;
    }
    m = depth;
    while (m > 0) {
        c = cmp(ud, &x, &h->items[path[m]]);
        if (c == DYN_HEAP_ABORT)
            return -1;
        if (c >= 0)
            break;
        m--;
    }
    for (c = 0; c < m; c++)
        h->items[path[c]] = h->items[path[c + 1]];
    h->items[path[m]] = x;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
