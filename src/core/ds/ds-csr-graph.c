#include "ds-internal.h"
#include "ds-csr-graph.h"
#include "ds-heap.h"

struct dyn_csr {
    uint32_t n;
    size_t m;
    uint32_t* row;
    uint32_t* col;
    double* w;
    int directed;
};

dyn_csr_t* dyn_csr_build(uint32_t nodes, const dyn_csr_edge* edges, size_t nedges, int directed)
{
    dyn_csr_t* g;
    uint32_t* cur = NULL;
    size_t i, m;
    if (nodes == UINT32_MAX)
        return NULL;
    for (i = 0; i < nedges; i++)
        if (edges[i].src >= nodes || edges[i].dst >= nodes || edges[i].w != edges[i].w)
            return NULL;
    m = directed ? nedges : nedges * 2;
    if (m > UINT32_MAX)
        return NULL;
    g = (dyn_csr_t*)DYN_DS_CALLOC(1, sizeof(*g));
    if (!g)
        return NULL;
    g->n = nodes;
    g->m = m;
    g->directed = directed;
    g->row = (uint32_t*)DYN_DS_CALLOC((size_t)nodes + 1, sizeof(uint32_t));
    g->col = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (m ? m : 1));
    g->w = (double*)DYN_DS_MALLOC(sizeof(double) * (m ? m : 1));
    cur = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)nodes + 1));
    if (!g->row || !g->col || !g->w || !cur)
        goto fail;
    for (i = 0; i < nedges; i++) {
        g->row[edges[i].src + 1]++;
        if (!directed)
            g->row[edges[i].dst + 1]++;
    }
    for (i = 0; i < nodes; i++)
        g->row[i + 1] += g->row[i];
    memcpy(cur, g->row, sizeof(uint32_t) * ((size_t)nodes + 1));
    for (i = 0; i < nedges; i++) {
        uint32_t p = cur[edges[i].src]++;
        g->col[p] = edges[i].dst;
        g->w[p] = edges[i].w;
        if (!directed) {
            p = cur[edges[i].dst]++;
            g->col[p] = edges[i].src;
            g->w[p] = edges[i].w;
        }
    }
    DYN_DS_FREE(cur);
    return g;
fail:
    DYN_DS_FREE(cur);
    dyn_csr_free(g);
    return NULL;
}

void dyn_csr_free(dyn_csr_t* g)
{
    if (!g)
        return;
    DYN_DS_FREE(g->row);
    DYN_DS_FREE(g->col);
    DYN_DS_FREE(g->w);
    DYN_DS_FREE(g);
}

uint32_t dyn_csr_nodes(const dyn_csr_t* g)
{
    return g->n;
}

size_t dyn_csr_edges(const dyn_csr_t* g)
{
    return g->m;
}

uint32_t dyn_csr_degree(const dyn_csr_t* g, uint32_t v)
{
    return v < g->n ? g->row[v + 1] - g->row[v] : 0;
}

const uint32_t* dyn_csr_neighbors(const dyn_csr_t* g, uint32_t v, const double** w, uint32_t* count)
{
    if (v >= g->n) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = g->row[v + 1] - g->row[v];
    if (w)
        *w = g->w + g->row[v];
    return g->col + g->row[v];
}

int dyn_csr_bfs(const dyn_csr_t* g, uint32_t src, int32_t* dist)
{
    uint32_t *q, head = 0, tail = 0, i;
    if (src >= g->n)
        return -1;
    q = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (g->n ? g->n : 1));
    if (!q)
        return -1;
    for (i = 0; i < g->n; i++)
        dist[i] = -1;
    dist[src] = 0;
    q[tail++] = src;
    while (head < tail) {
        uint32_t u = q[head++], e;
        for (e = g->row[u]; e < g->row[u + 1]; e++) {
            uint32_t v = g->col[e];
            if (dist[v] < 0) {
                dist[v] = dist[u] + 1;
                q[tail++] = v;
            }
        }
    }
    DYN_DS_FREE(q);
    return 0;
}

int dyn_csr_dijkstra(const dyn_csr_t* g, uint32_t src, double* dist, int32_t* pred)
{
    ds_heap_item* h;
    size_t hn = 0, i, n;
    if (src >= g->n)
        return -1;
    for (i = 0; i < g->m; i++)
        if (g->w[i] < 0)
            return -1;
    h = (ds_heap_item*)DYN_DS_MALLOC(sizeof(ds_heap_item) * (g->m + 1));
    if (!h)
        return -1;
    n = g->n;
    for (i = 0; i < n; i++)
        dist[i] = (double)INFINITY;
    if (pred)
        for (i = 0; i < n; i++)
            pred[i] = -1;
    dist[src] = 0;
    ds_heap_push(h, &hn, (ds_heap_item) { 0, src });
    while (hn) {
        ds_heap_item it = ds_heap_pop(h, &hn);
        uint32_t e;
        if (it.d > dist[it.v])
            continue;
        for (e = g->row[it.v]; e < g->row[it.v + 1]; e++) {
            uint32_t v = g->col[e];
            double nd = it.d + g->w[e];
            if (nd < dist[v]) {
                dist[v] = nd;
                if (pred)
                    pred[v] = (int32_t)it.v;
                ds_heap_push(h, &hn, (ds_heap_item) { nd, v });
            }
        }
    }
    DYN_DS_FREE(h);
    return 0;
}

int dyn_csr_pagerank(const dyn_csr_t* g, double damping, uint32_t iters, double tol, double* rank)
{
    double *next, inv;
    uint32_t it, i;
    if (g->n == 0)
        return 0;
    if (!(damping >= 0 && damping <= 1))
        return -1;
    next = (double*)DYN_DS_MALLOC(sizeof(double) * g->n);
    if (!next)
        return -1;
    inv = 1.0 / g->n;
    for (i = 0; i < g->n; i++)
        rank[i] = inv;
    for (it = 0; it < iters; it++) {
        double dangling = 0, diff = 0;
        for (i = 0; i < g->n; i++)
            next[i] = 0;
        for (i = 0; i < g->n; i++) {
            uint32_t deg = g->row[i + 1] - g->row[i], e;
            if (!deg) {
                dangling += rank[i];
                continue;
            }
            for (e = g->row[i]; e < g->row[i + 1]; e++)
                next[g->col[e]] += rank[i] / deg;
        }
        for (i = 0; i < g->n; i++) {
            double r = (1 - damping) * inv + damping * (next[i] + dangling * inv);
            diff += fabs(r - rank[i]);
            rank[i] = r;
        }
        if (diff < tol)
            break;
    }
    DYN_DS_FREE(next);
    return (int)it;
}

uint32_t dyn_csr_components(const dyn_csr_t* g, uint32_t* comp)
{
    uint32_t *p = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (g->n ? g->n : 1)), i, k = 0, e;
    if (!p)
        return UINT32_MAX;
    for (i = 0; i < g->n; i++)
        p[i] = i;
    for (i = 0; i < g->n; i++)
        for (e = g->row[i]; e < g->row[i + 1]; e++) {
            uint32_t a = ds_uf_find(p, i), b = ds_uf_find(p, g->col[e]);
            if (a != b)
                p[a < b ? b : a] = a < b ? a : b;
        }
    for (i = 0; i < g->n; i++)
        comp[i] = UINT32_MAX;
    for (i = 0; i < g->n; i++) {
        uint32_t r = ds_uf_find(p, i);
        if (comp[r] == UINT32_MAX)
            comp[r] = k++;
        comp[i] = comp[r];
    }
    DYN_DS_FREE(p);
    return k;
}

dyn_csr_t* dyn_csr_alloc(uint32_t nodes, size_t arcs, uint32_t** row, uint32_t** col, double** w)
{
    dyn_csr_t* g;
    if (nodes == UINT32_MAX || arcs > UINT32_MAX)
        return NULL;
    g = (dyn_csr_t*)DYN_DS_CALLOC(1, sizeof(*g));
    if (!g)
        return NULL;
    g->n = nodes;
    g->m = arcs;
    g->directed = 1;
    g->row = (uint32_t*)DYN_DS_CALLOC((size_t)nodes + 1, sizeof(uint32_t));
    g->col = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (arcs ? arcs : 1));
    g->w = (double*)DYN_DS_MALLOC(sizeof(double) * (arcs ? arcs : 1));
    if (!g->row || !g->col || !g->w) {
        dyn_csr_free(g);
        return NULL;
    }
    *row = g->row;
    *col = g->col;
    *w = g->w;
    return g;
}
