#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-union-find.h"

uint32_t ds_uf_find(uint32_t* p, uint32_t x)
{
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

int dyn_uf_init(dyn_uf_t* u, uint32_t n)
{
    uint32_t i;
    u->n = n;
    u->sets = n;
    u->parent = n ? (uint32_t*)DYN_DS_MALLOC((size_t)n * sizeof(uint32_t)) : NULL;
    u->rank = n ? (uint8_t*)DYN_DS_CALLOC(n, 1) : NULL;
    if (n && (!u->parent || !u->rank)) {
        dyn_uf_free(u);
        return -1;
    }
    for (i = 0; i < n; i++)
        u->parent[i] = i;
    return 0;
}

void dyn_uf_free(dyn_uf_t* u)
{
    DYN_DS_FREE(u->parent);
    DYN_DS_FREE(u->rank);
    u->parent = NULL;
    u->rank = NULL;
    u->n = u->sets = 0;
}

int dyn_uf_union(dyn_uf_t* u, uint32_t x, uint32_t y)
{
    uint32_t rx = dyn_uf_find(u, x), ry = dyn_uf_find(u, y);
    if (rx == ry)
        return 0;
    if (u->rank[rx] < u->rank[ry]) {
        uint32_t t = rx;
        rx = ry;
        ry = t;
    }
    u->parent[ry] = rx;
    if (u->rank[rx] == u->rank[ry])
        u->rank[rx]++;
    u->sets--;
    return 1;
}

int dyn_uf_validate(const dyn_uf_t* u, uint32_t* nroots)
{
    uint32_t n = u->n, i, roots = 0;
    uint8_t* st = (uint8_t*)DYN_DS_CALLOC(n ? n : 1, 1);
    if (!st)
        return -1;
    for (i = 0; i < n; i++) {
        uint32_t x = i, y;
        int cyc = 0;
        while (st[x] == 0 && u->parent[x] != x) {
            st[x] = 1;
            x = u->parent[x];
        }
        if (st[x] == 1 && u->parent[x] != x)
            cyc = 1;
        y = i;
        while (st[y] == 1) {
            st[y] = cyc ? 0 : 2;
            y = u->parent[y];
        }
        if (cyc) {
            DYN_DS_FREE(st);
            return 1;
        }
    }
    for (i = 0; i < n; i++)
        if (u->parent[i] == i)
            roots++;
    DYN_DS_FREE(st);
    *nroots = roots;
    return 0;
}
