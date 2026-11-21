#include "graphk-int.h"

int64_t dyn_gk_bfs(const dyn_gk_csr_t* g, uint32_t src, int* order)
{
    int* q;
    char* seen;
    uint32_t head = 0, tail = 0, no = 0, e;
    q = (int*)malloc((size_t)g->n * sizeof(int));
    seen = (char*)calloc(g->n, 1);
    if (!q || !seen) {
        free(q);
        free(seen);
        return DYN_GK_E_OOM;
    }
    q[tail++] = (int)src;
    seen[src] = 1;
    while (head < tail) {
        int u = q[head++];
        order[no++] = u;
        for (e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->tgt[e];
            if (!seen[v]) {
                seen[v] = 1;
                q[tail++] = (int)v;
            }
        }
    }
    free(q);
    free(seen);
    return no;
}

int64_t dyn_gk_dfs(const dyn_gk_csr_t* g, uint32_t src, int* order)
{
    int* stack;
    char* seen;
    uint32_t sp = 0, no = 0, e;
    uint64_t cap = (uint64_t)g->m + 1;
    if (cap > (uint64_t)UINT32_MAX)
        return DYN_GK_E_TOO_LARGE;
    stack = (int*)malloc((size_t)cap * sizeof(int));
    seen = (char*)calloc(g->n, 1);
    if (!stack || !seen) {
        free(stack);
        free(seen);
        return DYN_GK_E_OOM;
    }
    stack[sp++] = (int)src;
    while (sp) {
        int u = stack[--sp];
        if (seen[u])
            continue;
        seen[u] = 1;
        order[no++] = u;
        for (e = g->off[u + 1]; e-- > g->off[u];) {
            uint32_t v = g->tgt[e];
            if (!seen[v])
                stack[sp++] = (int)v;
        }
    }
    free(stack);
    free(seen);
    return no;
}

int64_t dyn_gk_bfs_adj(const dyn_graph_t* g, uint32_t src, int* order)
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
            if (!seen[v]) {
                seen[v] = 1;
                order[tail++] = (int)v;
            }
        }
    }
    free(seen);
    return tail;
}

int64_t dyn_gk_dfs_adj(const dyn_graph_t* g, uint32_t src, int* order)
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
            if (!seen[v])
                stack[sp++] = (int)v;
        }
    }
    free(stack);
    free(seen);
    return no;
}

static int dyn_gk_emit(dyn_gk_visit_fn fn, void* ud, int x)
{
    int ev = fn(ud, x);
    if (ev == DYN_GK_E_MUTATED)
        return DYN_GK_E_MUTATED;
    if (ev < 0)
        return DYN_GK_E_CALLBACK;
    return ev == 0 ? 0 : 1;
}

int dyn_gk_bfs_visit(const dyn_gk_csr_t* g, uint32_t src, dyn_gk_visit_fn fn, void* ud, uint32_t* pvisited)
{
    int* q;
    char* seen;
    uint32_t head = 0, tail = 0, visited = 0, e;
    int ev;

    q = (int*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    seen = (char*)calloc(g->n ? g->n : 1, 1);
    if (!q || !seen) {
        free(q);
        free(seen);
        return DYN_GK_E_OOM;
    }
    q[tail++] = (int)src;
    seen[src] = 1;
    while (head < tail) {
        int x = q[head++];
        visited++;
        ev = dyn_gk_emit(fn, ud, x);
        if (ev < 0) {
            free(q);
            free(seen);
            return ev;
        }
        if (ev == 0)
            break;
        for (e = g->off[x]; e < g->off[x + 1]; e++) {
            uint32_t v = g->tgt[e];
            if (!seen[v]) {
                seen[v] = 1;
                q[tail++] = (int)v;
            }
        }
    }
    free(q);
    free(seen);
    *pvisited = visited;
    return DYN_GK_OK;
}

int dyn_gk_dfs_visit(const dyn_gk_csr_t* g, uint32_t src, dyn_gk_visit_fn fn, void* ud, uint32_t* pvisited)
{
    int* stack;
    char* seen;
    uint32_t sp = 0, visited = 0, e;
    int ev;
    uint64_t cap = (uint64_t)g->m + 1;

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
        int x = stack[--sp];
        if (seen[x])
            continue;
        seen[x] = 1;
        visited++;
        ev = dyn_gk_emit(fn, ud, x);
        if (ev < 0) {
            free(stack);
            free(seen);
            return ev;
        }
        if (ev == 0)
            break;
        for (e = g->off[x + 1]; e-- > g->off[x];) {
            uint32_t v = g->tgt[e];
            if (!seen[v])
                stack[sp++] = (int)v;
        }
    }
    free(stack);
    free(seen);
    *pvisited = visited;
    return DYN_GK_OK;
}

static int dyn_gk_indeg(const dyn_gk_csr_t* g, int** pindeg, int** pq, uint32_t* ptail)
{
    int *indeg, *q;
    uint32_t u, e, tail = 0;
    indeg = (int*)calloc(g->n ? g->n : 1, sizeof(int));
    q = (int*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    if (!indeg || !q) {
        free(indeg);
        free(q);
        return DYN_GK_E_OOM;
    }
    for (e = 0; e < g->m; e++)
        indeg[g->tgt[e]]++;
    for (u = 0; u < g->n; u++)
        if (indeg[u] == 0)
            q[tail++] = (int)u;
    *pindeg = indeg;
    *pq = q;
    *ptail = tail;
    return DYN_GK_OK;
}

int64_t dyn_gk_topo(const dyn_gk_csr_t* g, int* order)
{
    int *indeg, *q;
    uint32_t head = 0, tail, no = 0, e;
    if (dyn_gk_indeg(g, &indeg, &q, &tail) < 0)
        return DYN_GK_E_OOM;
    while (head < tail) {
        int x = q[head++];
        order[no++] = x;
        for (e = g->off[x]; e < g->off[x + 1]; e++) {
            uint32_t v = g->tgt[e];
            if (--indeg[v] == 0)
                q[tail++] = (int)v;
        }
    }
    free(indeg);
    free(q);
    if (no != g->n)
        return DYN_GK_E_CYCLE;
    return no;
}

int dyn_gk_topo_visit(const dyn_gk_csr_t* g, dyn_gk_visit_fn fn, void* ud, uint32_t* pvisited)
{
    int *indeg, *q;
    uint32_t head = 0, tail, visited = 0, e;
    int ev;

    if (dyn_gk_indeg(g, &indeg, &q, &tail) < 0)
        return DYN_GK_E_OOM;
    while (head < tail) {
        int x = q[head++];
        visited++;
        ev = dyn_gk_emit(fn, ud, x);
        if (ev < 0) {
            free(indeg);
            free(q);
            return ev;
        }
        if (ev == 0) {
            free(indeg);
            free(q);
            *pvisited = visited;
            return DYN_GK_OK;
        }
        for (e = g->off[x]; e < g->off[x + 1]; e++) {
            uint32_t v = g->tgt[e];
            if (--indeg[v] == 0)
                q[tail++] = (int)v;
        }
    }
    free(indeg);
    free(q);
    if (visited < g->n)
        return DYN_GK_E_CYCLE;
    *pvisited = visited;
    return DYN_GK_OK;
}
