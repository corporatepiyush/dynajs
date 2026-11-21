#ifndef DS_UNION_FIND_H
#define DS_UNION_FIND_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t* parent;
    uint8_t* rank;
    uint32_t n;
    uint32_t sets;
} dyn_uf_t;

int dyn_uf_init(dyn_uf_t* u, uint32_t n);

void dyn_uf_free(dyn_uf_t* u);

int dyn_uf_union(dyn_uf_t* u, uint32_t x, uint32_t y);

int dyn_uf_validate(const dyn_uf_t* u, uint32_t* nroots);

static inline uint32_t dyn_uf_find(dyn_uf_t* u, uint32_t x)
{
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]];
        x = u->parent[x];
    }
    return x;
}

#ifdef __cplusplus
}
#endif

#endif
