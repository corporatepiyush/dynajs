#include "graphk.h"

dyn_graph_t* dyn_gk_new(int directed, int weighted)
{
    dyn_graph_t* g = (dyn_graph_t*)malloc(sizeof(*g));
    if (!g)
        return NULL;
    g->adj = NULL;
    g->n = g->cap = 0;
    g->m = 0;
    g->typed = 0;
    g->directed = directed;
    g->weighted = weighted;
    return g;
}

void dyn_gk_free(void* native)
{
    dyn_graph_t* g = (dyn_graph_t*)native;
    uint32_t i;
    if (!g)
        return;
    for (i = 0; i < g->n; i++)
        free(g->adj[i].e);
    free(g->adj);
    free(g);
}

int dyn_gk_ensure(dyn_graph_t* g, uint32_t id)
{
    if (id >= g->cap) {
        uint32_t ncap = g->cap ? g->cap : 8;
        dyn_adj_t* na;
        while (ncap <= id) {
            if (ncap > UINT32_MAX / 2)
                return -1;
            ncap *= 2;
        }
#if SIZE_MAX < UINT64_MAX
        if (ncap > SIZE_MAX / sizeof(dyn_adj_t))
            return -1;
#endif
        na = (dyn_adj_t*)realloc(g->adj, (size_t)ncap * sizeof(dyn_adj_t));
        if (!na)
            return -1;
        g->adj = na;
        g->cap = ncap;
    }
    while (g->n <= id) {
        g->adj[g->n].e = NULL;
        g->adj[g->n].n = 0;
        g->adj[g->n].cap = 0;
        g->n++;
    }
    return 0;
}

static int dyn_gk_push(dyn_adj_t* a, int to, double w, uint8_t t)
{
    if (a->n == a->cap) {
        uint32_t nc;
        dyn_edge_t* ne;
        if (a->cap > UINT32_MAX / 2)
            return -1;
        nc = a->cap ? a->cap * 2 : 4;
#if SIZE_MAX < UINT64_MAX
        if (nc > SIZE_MAX / sizeof(dyn_edge_t))
            return -1;
#endif
        ne = (dyn_edge_t*)realloc(a->e, (size_t)nc * sizeof(dyn_edge_t));
        if (!ne)
            return -1;
        a->e = ne;
        a->cap = nc;
    }
    a->e[a->n].to = to;
    a->e[a->n].t = t;
    a->e[a->n].w = w;
    a->n++;
    return 0;
}

int dyn_gk_add_edge(dyn_graph_t* g, uint32_t u, uint32_t v, double w)
{
    return dyn_gk_add_edge_typed(g, u, v, w, 0);
}

int dyn_gk_add_edge_typed(dyn_graph_t* g, uint32_t u, uint32_t v, double w, uint8_t t)
{
    if (!g->weighted)
        w = 1.0;
    if (dyn_gk_ensure(g, u > v ? u : v))
        return DYN_GK_E_OOM;
    if (dyn_gk_push(&g->adj[u], (int)v, w, t))
        return DYN_GK_E_OOM;
    if (!g->directed && u != v) {
        if (dyn_gk_push(&g->adj[v], (int)u, w, t))
            return DYN_GK_E_OOM;
    }
    g->m++;
    if (t)
        g->typed++;
    return DYN_GK_OK;
}

int dyn_gk_has_edge(const dyn_graph_t* g, uint32_t u, uint32_t v)
{
    uint32_t i;
    if (u >= g->n)
        return 0;
    for (i = 0; i < g->adj[u].n; i++)
        if ((uint32_t)g->adj[u].e[i].to == v)
            return 1;
    return 0;
}

int dyn_gk_has_negative(const dyn_graph_t* g)
{
    uint32_t u, i;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            if (g->adj[u].e[i].w < 0.0)
                return 1;
    return 0;
}

int dyn_gk_export_csr(const dyn_graph_t* g, int32_t** poff, int32_t** ped, uint64_t* ptotal)
{
    int32_t *off, *ed;
    uint64_t total = 0;
    uint32_t u, i, at = 0;
    *poff = *ped = NULL;
    for (u = 0; u < g->n; u++)
        total += g->adj[u].n;
    *ptotal = total;
    if (total > (uint64_t)INT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    off = (int32_t*)malloc(((size_t)g->n + 1) * sizeof(int32_t));
    ed = (int32_t*)malloc((total ? (size_t)total : 1) * sizeof(int32_t));
    if (!off || !ed) {
        free(off);
        free(ed);
        return DYN_GK_E_OOM;
    }
    for (u = 0; u < g->n; u++) {
        off[u] = (int32_t)at;
        for (i = 0; i < g->adj[u].n; i++)
            ed[at++] = g->adj[u].e[i].to;
    }
    off[g->n] = (int32_t)at;
    *poff = off;
    *ped = ed;
    return DYN_GK_OK;
}
