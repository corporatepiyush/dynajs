#include "graphk-int.h"

static inline int dyn_gk_allowed(uint32_t mask, uint8_t t)
{
    return (int)((mask >> t) & 1u);
}

int dyn_gk_csr_of_graph_mask(const dyn_graph_t* g, int with_w, uint32_t mask, dyn_gk_csr_t* out)
{
    uint64_t m = 0;
    uint32_t u, i, at = 0;
    int rc;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            m += (uint64_t)dyn_gk_allowed(mask, g->adj[u].e[i].t);
    rc = dyn_gk_csr_alloc(out, g->n, m, with_w);
    if (rc < 0)
        return rc;
    for (u = 0; u < g->n; u++) {
        const dyn_edge_t* e = g->adj[u].e;
        out->off[u] = at;
        for (i = 0; i < g->adj[u].n; i++) {
            if (!dyn_gk_allowed(mask, e[i].t))
                continue;
            out->tgt[at] = (uint32_t)e[i].to;
            if (with_w)
                out->w[at] = e[i].w;
            at++;
        }
    }
    out->off[g->n] = at;
    return DYN_GK_OK;
}

int64_t dyn_gk_bfs_mask(const dyn_graph_t* g, uint32_t src, uint32_t mask, int* order)
{
    char* seen = (char*)calloc(g->n ? g->n : 1, 1);
    uint32_t head = 0, tail = 0, i;
    if (!seen)
        return DYN_GK_E_OOM;
    order[tail++] = (int)src;
    seen[src] = 1;
    while (head < tail) {
        const dyn_adj_t* a = &g->adj[order[head++]];
        for (i = 0; i < a->n; i++) {
            uint32_t v = (uint32_t)a->e[i].to;
            if (!seen[v] && dyn_gk_allowed(mask, a->e[i].t)) {
                seen[v] = 1;
                order[tail++] = (int)v;
            }
        }
    }
    free(seen);
    return tail;
}

int64_t dyn_gk_dfs_mask(const dyn_graph_t* g, uint32_t src, uint32_t mask, int* order)
{
    int* stack;
    char* seen;
    uint32_t sp = 0, no = 0, u, i;
    uint64_t cap = 1;
    for (u = 0; u < g->n; u++)
        cap += g->adj[u].n;
    if (cap > (uint64_t)UINT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    stack = (int*)malloc((size_t)cap * sizeof(int));
    seen = (char*)calloc(g->n ? g->n : 1, 1);
    if (!stack || !seen) {
        free(stack);
        free(seen);
        return DYN_GK_E_OOM;
    }
    stack[sp++] = (int)src;
    while (sp) {
        const dyn_adj_t* a;
        int x = stack[--sp];
        if (seen[x])
            continue;
        seen[x] = 1;
        order[no++] = x;
        a = &g->adj[x];
        for (i = a->n; i-- > 0;) {
            uint32_t v = (uint32_t)a->e[i].to;
            if (!seen[v] && dyn_gk_allowed(mask, a->e[i].t))
                stack[sp++] = (int)v;
        }
    }
    free(stack);
    free(seen);
    return no;
}

int64_t dyn_gk_khop(const dyn_graph_t* g, uint32_t src, uint32_t k, uint32_t mask, int32_t* order, int32_t* dist)
{
    int32_t* d = (int32_t*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int32_t));
    uint32_t head = 0, tail = 0, i;
    if (!d)
        return DYN_GK_E_OOM;
    for (i = 0; i < g->n; i++)
        d[i] = -1;
    order[tail++] = (int32_t)src;
    d[src] = 0;
    while (head < tail) {
        uint32_t u = (uint32_t)order[head++];
        const dyn_adj_t* a = &g->adj[u];
        if ((uint32_t)d[u] >= k)
            continue;
        for (i = 0; i < a->n; i++) {
            uint32_t v = (uint32_t)a->e[i].to;
            if (d[v] < 0 && dyn_gk_allowed(mask, a->e[i].t)) {
                d[v] = d[u] + 1;
                order[tail++] = (int32_t)v;
            }
        }
    }
    for (i = 0; i < tail; i++)
        dist[i] = d[order[i]];
    free(d);
    return tail;
}

int64_t dyn_gk_path_ends(const dyn_graph_t* g, const uint32_t* src, uint32_t nsrc, const uint32_t* masks, uint32_t hops, int32_t* out)
{
    size_t n = g->n ? g->n : 1;
    uint8_t* mark = (uint8_t*)calloc(n, 1);
    uint32_t* cur = (uint32_t*)malloc(n * sizeof(uint32_t));
    uint32_t* nxt = (uint32_t*)malloc(n * sizeof(uint32_t));
    uint32_t nc = 0, nn, h, i, j, u;
    int64_t cnt = 0;
    if (!mark || !cur || !nxt) {
        free(mark);
        free(cur);
        free(nxt);
        return DYN_GK_E_OOM;
    }
    for (i = 0; i < nsrc; i++)
        if (!mark[src[i]]) {
            mark[src[i]] = 1;
            cur[nc++] = src[i];
        }
    for (h = 0; h < hops && nc; h++) {
        uint32_t* t;
        for (i = 0; i < nc; i++)
            mark[cur[i]] = 0;
        nn = 0;
        for (i = 0; i < nc; i++) {
            const dyn_adj_t* a = &g->adj[cur[i]];
            for (j = 0; j < a->n; j++) {
                uint32_t v = (uint32_t)a->e[j].to;
                if (!mark[v] && dyn_gk_allowed(masks[h], a->e[j].t)) {
                    mark[v] = 1;
                    nxt[nn++] = v;
                }
            }
        }
        t = cur;
        cur = nxt;
        nxt = t;
        nc = nn;
    }
    if (nc)
        for (u = 0; u < g->n; u++)
            if (mark[u])
                out[cnt++] = (int32_t)u;
    free(mark);
    free(cur);
    free(nxt);
    return cnt;
}

int dyn_gk_paths(const dyn_graph_t* g, const uint32_t* src, uint32_t nsrc, const uint32_t* masks, uint32_t hops, int32_t* nodes, uint8_t* types, uint64_t cap, uint64_t budget, uint64_t* ptotal)
{
    uint32_t* it = (uint32_t*)malloc(((size_t)hops + 1) * sizeof(uint32_t));
    int32_t* path = (int32_t*)malloc(((size_t)hops + 1) * sizeof(int32_t));
    uint8_t* tp = (uint8_t*)malloc((size_t)hops + 1);
    uint64_t total = 0, steps = 0;
    uint32_t s, d;
    int rc = DYN_GK_OK;
    if (!it || !path || !tp) {
        rc = DYN_GK_E_OOM;
        goto done;
    }
    for (s = 0; s < nsrc; s++) {
        path[0] = (int32_t)src[s];
        d = 0;
        it[0] = 0;
        if (hops == 0) {
            if (total < cap)
                nodes[total] = path[0];
            total++;
            continue;
        }
        for (;;) {
            const dyn_adj_t* a = &g->adj[path[d]];
            const dyn_edge_t* e;
            if (it[d] >= a->n) {
                if (d == 0)
                    break;
                d--;
                continue;
            }
            e = &a->e[it[d]++];
            if (++steps > budget) {
                rc = DYN_GK_E_BUDGET;
                goto done;
            }
            if (!dyn_gk_allowed(masks[d], e->t))
                continue;
            tp[d] = e->t;
            path[d + 1] = e->to;
            if (d + 1 < hops) {
                d++;
                it[d] = 0;
                continue;
            }
            if (total < cap) {
                memcpy(nodes + total * ((uint64_t)hops + 1), path, ((size_t)hops + 1) * sizeof(int32_t));
                if (types)
                    memcpy(types + total * hops, tp, hops);
            }
            total++;
        }
    }
done:
    *ptotal = total;
    free(it);
    free(path);
    free(tp);
    return rc;
}
