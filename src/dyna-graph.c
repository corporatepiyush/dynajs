/*
 * Graph -- a native Graph with traversal / shortest-path / spanning-tree
 * algorithms as METHODS (the heavy work is a single JS->C transition + a C loop,
 * never N interpreted steps).
 *
 *   import { Graph } from "dyna:structures";
 *   const g = new Graph({ directed: true, weighted: true });
 *   const a = g.addNode(), b = g.addNode(), c = g.addNode();
 *   g.addEdge(a, b, 2).addEdge(b, c, 3);
 *   g.dijkstra(a);                 // [0, 2, 5]
 *
 * Nodes are integer ids 0..n-1 (addNode returns the next id; addEdge auto-grows
 * to include its endpoints). Like every other dyna:structures class the Graph is
 * a PLAIN GC-managed object -- no .close(): it holds no JSValues, so a finalizer
 * frees its native adjacency arrays when it becomes unreachable. Every result is
 * copied into fresh JS arrays/objects; nothing native escapes.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#include "dyna-serialize.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct { int to; double w; } dyn_edge_t;
typedef struct { dyn_edge_t *e; uint32_t n, cap; } dyn_adj_t;

typedef struct {
    dyn_adj_t *adj;
    uint32_t n;        /* node count */
    uint32_t cap;      /* adj capacity */
    uint64_t m;        /* logical edge count */
    int directed;
    int weighted;
} dyn_graph_t;

static void dyn_graph_free(void *native)
{
    dyn_graph_t *g = (dyn_graph_t *)native;
    uint32_t i;
    if (!g)
        return;
    for (i = 0; i < g->n; i++)
        free(g->adj[i].e);
    free(g->adj);
    free(g);
}

static JSClassID dyn_graph_class_id;
static void dyn_graph_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_graph_free(JS_GetOpaque(val, dyn_graph_class_id));
}
static const JSClassDef dyn_graph_class = {
    "Graph",
    .finalizer = dyn_graph_finalizer,
};

/* Grow node array so `id` is a valid node, adding isolated nodes. 0 or -1. */
static int dyn_graph_ensure(dyn_graph_t *g, uint32_t id)
{
    if (id >= g->cap) {
        uint32_t ncap = g->cap ? g->cap : 8;
        dyn_adj_t *na;
        while (ncap <= id) {
            if (ncap > UINT32_MAX / 2)
                return -1;
            ncap *= 2;
        }
        na = (dyn_adj_t *)realloc(g->adj, (size_t)ncap * sizeof(dyn_adj_t));
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

static int dyn_adj_push(dyn_adj_t *a, int to, double w)
{
    if (a->n == a->cap) {
        uint32_t nc;
        dyn_edge_t *ne;
        if (a->cap > UINT32_MAX / 2)
            return -1;
        nc = a->cap ? a->cap * 2 : 4;
        ne = (dyn_edge_t *)realloc(a->e, (size_t)nc * sizeof(dyn_edge_t));
        if (!ne)
            return -1;
        a->e = ne;
        a->cap = nc;
    }
    a->e[a->n].to = to;
    a->e[a->n].w = w;
    a->n++;
    return 0;
}

static JSValue dyn_graph_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    int directed = 0, weighted = 0;
    JSValue obj;

    (void)new_target;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue d = JS_GetPropertyStr(ctx, argv[0], "directed");
        JSValue w = JS_GetPropertyStr(ctx, argv[0], "weighted");
        if (JS_IsException(d) || JS_IsException(w)) {
            JS_FreeValue(ctx, d); JS_FreeValue(ctx, w);
            return JS_EXCEPTION;
        }
        directed = JS_ToBool(ctx, d);
        weighted = JS_ToBool(ctx, w);
        JS_FreeValue(ctx, d); JS_FreeValue(ctx, w);
    }
    g = (dyn_graph_t *)malloc(sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->adj = NULL;
    g->n = g->cap = 0;
    g->m = 0;
    g->directed = directed;
    g->weighted = weighted;
    obj = dyn_plain_wrap(ctx, dyn_graph_class_id, g, dyn_graph_free);
    return obj;
}

static dyn_graph_t *graph_of(JSContext *ctx, JSValueConst t)
{
    return (dyn_graph_t *)dyn_plain_get(ctx, t, dyn_graph_class_id);
}

/* addNode(): append an isolated node, return its id. */
static JSValue dyn_graph_add_node(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    (void)argc; (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (dyn_graph_ensure(g, g->n))
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt64(ctx, (int64_t)(g->n - 1));
}

/* addEdge(u, v[, w]): add an edge (both directions if undirected). */
static JSValue dyn_graph_add_edge(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t u, v;
    double w = 1.0;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &v))
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &w, argv[2]))
        return JS_EXCEPTION;
    /* NaN/+-Inf compares false everywhere, so a non-finite weight makes every
     * algorithm silently produce garbage (dijkstra never relaxes, mst sorts
     * it arbitrarily). Refuse it like every other numeric-typed module does. */
    if (!isfinite(w))
        return JS_ThrowRangeError(ctx, "edge weight must be finite");
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!g->weighted)
        w = 1.0;
    if (dyn_graph_ensure(g, (uint32_t)(u > v ? u : v)))
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_adj_push(&g->adj[u], v, w))
        return JS_ThrowOutOfMemory(ctx);
    if (!g->directed && u != v) {
        if (dyn_adj_push(&g->adj[v], u, w))
            return JS_ThrowOutOfMemory(ctx);
    }
    g->m++;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_graph_neighbors(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t u;
    JSValue arr;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (u >= g->n)
        return JS_ThrowRangeError(ctx, "node out of range");
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < g->adj[u].n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, i,
                JS_NewInt64(ctx, g->adj[u].e[i].to), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_graph_has_edge(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t u, v;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &v))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (u >= g->n)
        return JS_NewBool(ctx, 0);
    for (i = 0; i < g->adj[u].n; i++)
        if (g->adj[u].e[i].to == v)
            return JS_NewBool(ctx, 1);
    return JS_NewBool(ctx, 0);
}

static JSValue dyn_graph_node_count(JSContext *ctx, JSValueConst this_val)
{
    dyn_graph_t *g = graph_of(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->n);
}

static JSValue dyn_graph_edge_count(JSContext *ctx, JSValueConst this_val)
{
    dyn_graph_t *g = graph_of(ctx, this_val);
    if (!g) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->m);
}

/* ---- result builders ---- */
static JSValue int_array(JSContext *ctx, const int *a, uint32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    if (JS_IsException(arr)) return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewInt64(ctx, a[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr); return JS_EXCEPTION;
        }
    return arr;
}
static JSValue dbl_array(JSContext *ctx, const double *a, uint32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    if (JS_IsException(arr)) return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewFloat64(ctx, a[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr); return JS_EXCEPTION;
        }
    return arr;
}

/* ---- BFS / DFS ---- */
static JSValue dyn_graph_bfs(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t src;
    int *order, *q;
    char *seen;
    uint32_t head = 0, tail = 0, no = 0, i;
    JSValue res;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n)
        return JS_ThrowRangeError(ctx, "source out of range");
    order = (int *)malloc((size_t)g->n * sizeof(int));
    q = (int *)malloc((size_t)g->n * sizeof(int));
    seen = (char *)calloc(g->n, 1);
    if (!order || !q || !seen) { free(order); free(q); free(seen);
                                 return JS_ThrowOutOfMemory(ctx); }
    q[tail++] = src; seen[src] = 1;
    while (head < tail) {
        int u = q[head++];
        order[no++] = u;
        for (i = 0; i < g->adj[u].n; i++) {
            int v = g->adj[u].e[i].to;
            if (!seen[v]) { seen[v] = 1; q[tail++] = v; }
        }
    }
    res = int_array(ctx, order, no);
    free(order); free(q); free(seen);
    return res;
}

static JSValue dyn_graph_dfs(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t src;
    int *order, *stack;
    char *seen;
    uint32_t sp = 0, no = 0, i;
    JSValue res;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n)
        return JS_ThrowRangeError(ctx, "source out of range");
    /* A node is marked seen when POPPED, not when pushed, so it can sit on the
     * stack once per incoming edge. Sizing this by g->n overflows the heap on
     * any graph with more edges than nodes. */
    {
        uint64_t cap = 1;
        for (i = 0; i < g->n; i++)
            cap += g->adj[i].n;
        if (cap > (uint64_t)UINT32_MAX)
            return JS_ThrowRangeError(ctx, "graph too large to traverse");
        order = (int *)malloc((size_t)g->n * sizeof(int));
        stack = (int *)malloc((size_t)cap * sizeof(int));
        seen = (char *)calloc(g->n, 1);
        if (!order || !stack || !seen) { free(order); free(stack); free(seen);
                                         return JS_ThrowOutOfMemory(ctx); }
    }
    stack[sp++] = src;
    while (sp) {
        int u = stack[--sp];
        if (seen[u]) continue;
        seen[u] = 1;
        order[no++] = u;
        /* push neighbours in reverse so smaller ids are visited first */
        for (i = g->adj[u].n; i-- > 0; ) {
            int v = g->adj[u].e[i].to;
            if (!seen[v]) stack[sp++] = v;
        }
    }
    res = int_array(ctx, order, no);
    free(order); free(stack); free(seen);
    return res;
}

/* ---- binary min-heap over (key, node) for Dijkstra / A* ---- */
typedef struct { double key; int node; } dyn_hp_item;

/* 16 bytes: pointer + int with unavoidable tail padding. Pinned because a
   field added here silently makes it 24 and these are per-element. */
_Static_assert(sizeof(dyn_hp_item) <= 16, "dyn_hp_item grew past 16 bytes");
typedef struct { dyn_hp_item *a; uint32_t n, cap; } dyn_heap;
static int heap_push(dyn_heap *h, double key, int node)
{
    uint32_t i;
    if (h->n == h->cap) {
        if (h->cap > UINT32_MAX / 2)
            return -1;
        uint32_t nc = h->cap ? h->cap * 2 : 16;
        dyn_hp_item *na = (dyn_hp_item *)realloc(h->a, (size_t)nc * sizeof(*na));
        if (!na) return -1;
        h->a = na; h->cap = nc;
    }
    i = h->n++;
    h->a[i].key = key; h->a[i].node = node;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (h->a[p].key <= h->a[i].key) break;
        { dyn_hp_item t = h->a[p]; h->a[p] = h->a[i]; h->a[i] = t; }
        i = p;
    }
    return 0;
}
static int heap_pop(dyn_heap *h, dyn_hp_item *out)
{
    uint32_t i = 0;
    if (h->n == 0) return 0;
    *out = h->a[0];
    h->a[0] = h->a[--h->n];
    for (;;) {
        uint32_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < h->n && h->a[l].key < h->a[s].key) s = l;
        if (r < h->n && h->a[r].key < h->a[s].key) s = r;
        if (s == i) break;
        { dyn_hp_item t = h->a[s]; h->a[s] = h->a[i]; h->a[i] = t; }
        i = s;
    }
    return 1;
}

/* Core Dijkstra: fill dist[] (INFINITY unreachable). Returns 0, -1 OOM, -2 on a
 * negative edge weight (invalid for Dijkstra). */
static int dijkstra_core(dyn_graph_t *g, int src, double *dist)
{
    dyn_heap h = { NULL, 0, 0 };
    dyn_hp_item it;
    uint32_t i;
    for (i = 0; i < g->n; i++) dist[i] = INFINITY;
    dist[src] = 0.0;
    if (heap_push(&h, 0.0, src)) { free(h.a); return -1; }
    while (heap_pop(&h, &it)) {
        if (it.key > dist[it.node]) continue; /* stale */
        for (i = 0; i < g->adj[it.node].n; i++) {
            int v = g->adj[it.node].e[i].to;
            double w = g->adj[it.node].e[i].w;
            if (w < 0) { free(h.a); return -2; }
            if (it.key + w < dist[v]) {
                dist[v] = it.key + w;
                if (heap_push(&h, dist[v], v)) { free(h.a); return -1; }
            }
        }
    }
    free(h.a);
    return 0;
}

/* Any negative edge, anywhere. Dijkstra and aStar refuse the shape up front
 * rather than only when the search happens to reach it: the same graph used
 * to throw from one source and silently succeed from another. */
static int graph_has_negative(const dyn_graph_t *g)
{
    uint32_t u, i;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            if (g->adj[u].e[i].w < 0.0)
                return 1;
    return 0;
}

static JSValue dyn_graph_dijkstra(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t src, dst = 0;
    int have_dst = 0, rc;
    double *dist;
    JSValue res;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (dyn_idx_arg(ctx, argv[1], &dst))
            return JS_EXCEPTION;
        have_dst = 1;
    }
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n)
        return JS_ThrowRangeError(ctx, "source out of range");
    if (have_dst && dst >= g->n)
        return JS_ThrowRangeError(ctx, "destination out of range");
    if (graph_has_negative(g))
        return JS_ThrowRangeError(ctx, "dijkstra: negative edge weight");
    dist = (double *)malloc((size_t)g->n * sizeof(double));
    if (!dist)
        return JS_ThrowOutOfMemory(ctx);
    rc = dijkstra_core(g, src, dist);
    if (rc == -1) { free(dist); return JS_ThrowOutOfMemory(ctx); }
    if (rc == -2) { free(dist);
        return JS_ThrowRangeError(ctx, "dijkstra: negative edge weight"); }
    if (have_dst) { double d = dist[dst]; free(dist); return JS_NewFloat64(ctx, d); }
    res = dbl_array(ctx, dist, g->n);
    free(dist);
    return res;
}

/* Bellman-Ford: dist[] from src; detects a negative cycle. */
static JSValue dyn_graph_bellman_ford(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t src;
    double *dist;
    uint32_t iter, u, i;
    int changed;
    JSValue res;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n)
        return JS_ThrowRangeError(ctx, "source out of range");
    dist = (double *)malloc((size_t)g->n * sizeof(double));
    if (!dist)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < g->n; i++) dist[i] = INFINITY;
    dist[src] = 0.0;
    for (iter = 0; iter < g->n; iter++) { /* n passes; last detects a cycle */
        changed = 0;
        for (u = 0; u < g->n; u++) {
            if (dist[u] == INFINITY) continue;
            for (i = 0; i < g->adj[u].n; i++) {
                int v = g->adj[u].e[i].to;
                double nd = dist[u] + g->adj[u].e[i].w;
                if (nd == -INFINITY) {
                    /* Path-weight underflow is not a cycle: propagate the
                     * true -Inf distance instead of letting the detection
                     * pass below read it as "still improving" and throw a
                     * false negative-cycle. A REAL cycle of weights this
                     * large is indistinguishable, but no finite graph can
                     * express it anyway. */
                    dist[v] = -INFINITY;
                    changed = 1;
                    continue;
                }
                if (nd < dist[v]) {
                    if (iter == g->n - 1) { free(dist);
                        return JS_ThrowRangeError(ctx, "bellmanFord: negative cycle"); }
                    dist[v] = nd; changed = 1;
                }
            }
        }
        if (!changed) break;
    }
    res = dbl_array(ctx, dist, g->n);
    free(dist);
    return res;
}

/* topologicalSort (Kahn): directed only; throws on a cycle. */
static JSValue dyn_graph_topo_sort(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    int *indeg, *q, *order;
    uint32_t head = 0, tail = 0, no = 0, u, i;
    JSValue res;
    (void)argc; (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!g->directed)
        return JS_ThrowTypeError(ctx, "topologicalSort requires a directed graph");
    indeg = (int *)calloc(g->n, sizeof(int));
    q = (int *)malloc((size_t)g->n * sizeof(int));
    order = (int *)malloc((size_t)g->n * sizeof(int));
    if ((g->n && (!indeg || !q || !order))) { free(indeg); free(q); free(order);
                                              return JS_ThrowOutOfMemory(ctx); }
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++) indeg[g->adj[u].e[i].to]++;
    for (u = 0; u < g->n; u++) if (indeg[u] == 0) q[tail++] = u;
    while (head < tail) {
        int x = q[head++];
        order[no++] = x;
        for (i = 0; i < g->adj[x].n; i++) {
            int v = g->adj[x].e[i].to;
            if (--indeg[v] == 0) q[tail++] = v;
        }
    }
    free(indeg); free(q);
    if (no != g->n) { free(order);
        return JS_ThrowRangeError(ctx, "topologicalSort: graph has a cycle"); }
    res = int_array(ctx, order, no);
    free(order);
    return res;
}

/* connectedComponents (undirected): comp[i] = component id (0-based). */
static JSValue dyn_graph_components(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    int *comp, *stack;
    uint32_t sp, i, s, ncomp = 0;
    JSValue res;
    (void)argc; (void)argv;
    /* Deliberately no directed guard (unlike mst/topoSort): on a directed
     * graph this answers WEAK components, which is the useful reading of
     * "connected" there. Strong components are a different algorithm. */
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    comp = (int *)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    stack = (int *)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    if (!comp || !stack) { free(comp); free(stack); return JS_ThrowOutOfMemory(ctx); }
    for (i = 0; i < g->n; i++) comp[i] = -1;
    for (s = 0; s < g->n; s++) {
        if (comp[s] != -1) continue;
        sp = 0; stack[sp++] = s; comp[s] = ncomp;
        while (sp) {
            int u = stack[--sp];
            for (i = 0; i < g->adj[u].n; i++) {
                int v = g->adj[u].e[i].to;
                if (comp[v] == -1) { comp[v] = ncomp; stack[sp++] = v; }
            }
        }
        ncomp++;
    }
    res = int_array(ctx, comp, g->n);
    free(comp); free(stack);
    return res;
}

/* floydWarshall(): n x n distance matrix (Array of Arrays of doubles). */
#define DYN_FLOYD_MAX_N 1024

static JSValue dyn_graph_floyd(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    double *d;
    uint32_t n, i, j, k;
    JSValue res;
    (void)argc; (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    n = g->n;
    /* A LATENCY bound, not a memory one: the triple loop is n^3, so 4096 is
       6.9e10 inner iterations (order a minute, on the loop thread) where 1024
       is 1.1e9. The result also costs n^2 JSValues -- 268 MB at 4096 against
       the 134 MB C matrix the old cap was sized for. */
    if (n > DYN_FLOYD_MAX_N)
        return JS_ThrowRangeError(ctx,
            "floydWarshall: graph too large (n > %u); the algorithm is O(n^3)",
            (unsigned)DYN_FLOYD_MAX_N);
    d = (double *)malloc((size_t)n * n * sizeof(double));
    if (n && !d)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) d[i * n + j] = (i == j) ? 0.0 : INFINITY;
    for (i = 0; i < n; i++)
        for (j = 0; j < g->adj[i].n; j++) {
            int v = g->adj[i].e[j].to;
            double w = g->adj[i].e[j].w;
            if (w < d[i * n + v]) d[i * n + v] = w;
        }
    for (k = 0; k < n; k++)
        for (i = 0; i < n; i++) {
            if (d[i * n + k] == INFINITY) continue;
            for (j = 0; j < n; j++) {
                double nd = d[i * n + k] + d[k * n + j];
                if (nd < d[i * n + j]) d[i * n + j] = nd;
            }
        }
    /* A negative cycle drives a diagonal entry below zero. Without this
     * check floydWarshall returned a plausible-looking matrix where every
     * other algorithm throws (BF detects the cycle; Dijkstra and aStar
     * refuse negative edges outright). */
    for (i = 0; i < n; i++)
        if (d[i * n + i] < 0.0) {
            free(d);
            return JS_ThrowRangeError(ctx, "floydWarshall: negative cycle");
        }
    res = JS_NewArray(ctx);
    if (JS_IsException(res)) { free(d); return res; }
    for (i = 0; i < n; i++) {
        JSValue row = dbl_array(ctx, d + (size_t)i * n, n);
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, res); free(d); return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, res, i, row, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, row);
            JS_FreeValue(ctx, res); free(d); return JS_EXCEPTION;
        }
    }
    free(d);
    return res;
}

/* mst() (Kruskal, undirected): { weight, edges:[[u,v,w],...] } spanning forest. */
typedef struct { int u, v; double w; } kedge;

/* Ties keep insertion order in neither arm; MST weight is what is defined, and
 * which equal-weight edge is chosen is not. */
static int kedge_cmp(const void *a, const void *b)
{
    double x = ((const kedge *)a)->w, y = ((const kedge *)b)->w;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Measured: qsort won at EVERY size tried, on both sawtooth and ascending
 * weights, by 1.2x at 250 edges and 163x at 100k. An earlier note recorded
 * insertion sort winning on small inputs and that did not reproduce, so this
 * floor is a conservative nod to it -- below 64 edges the quadratic term is
 * a few thousand operations and neither arm is measurable. */
#define DYN_MST_QSORT_MIN 64

static JSValue dyn_graph_mst(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    kedge *es;
    uint32_t ne = 0, u, i, cap;
    int *parent;
    double total = 0.0;
    JSValue res, edges;
    uint32_t out = 0;
    (void)argc; (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (g->directed)
        return JS_ThrowTypeError(ctx, "mst requires an undirected graph");
    /* collect unique edges (u < v to dedup the two adjacency entries) */
    cap = 16;
    es = (kedge *)malloc(cap * sizeof(kedge));
    parent = (int *)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    if (!es || !parent) { free(es); free(parent); return JS_ThrowOutOfMemory(ctx); }
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++) {
            int v = g->adj[u].e[i].to;
            if ((uint32_t)v < u) continue;      /* keep one direction */
            if (ne == cap) {
                kedge *n2;
                if (cap > UINT32_MAX / 2) { free(es); free(parent);
                    return JS_ThrowOutOfMemory(ctx); }
                n2 = (kedge *)realloc(es, (cap *= 2) * sizeof(kedge));
                if (!n2) { free(es); free(parent); return JS_ThrowOutOfMemory(ctx); }
                es = n2;
            }
            es[ne].u = u; es[ne].v = v; es[ne].w = g->adj[u].e[i].w; ne++;
        }
    /* GATED, not replaced. An inline insertion sort has no call per compare
     * and wins on small inputs -- an earlier unconditional switch to qsort was
     * 5x slower there and got reverted. But it is O(E^2), and 20k nodes at
     * degree 5 measured 868 ms, so the large arm has to exist. */
    if (ne > DYN_MST_QSORT_MIN) {
        qsort(es, ne, sizeof(*es), kedge_cmp);
    } else {                                    /* inline, no call per compare */
        for (i = 1; i < ne; i++) {
            kedge key = es[i];
            uint32_t j = i;
            while (j > 0 && es[j - 1].w > key.w) { es[j] = es[j - 1]; j--; }
            es[j] = key;
        }
    }
    for (u = 0; u < g->n; u++) parent[u] = u;
    res = JS_NewObject(ctx);
    edges = JS_NewArray(ctx);
    if (JS_IsException(res) || JS_IsException(edges)) {
        JS_FreeValue(ctx, res); JS_FreeValue(ctx, edges);
        free(es); free(parent); return JS_EXCEPTION;
    }
    for (i = 0; i < ne; i++) {
        int a = es[i].u, b = es[i].v, ra, rb;
        for (ra = a; parent[ra] != ra; ra = parent[ra]) parent[ra] = parent[parent[ra]];
        for (rb = b; parent[rb] != rb; rb = parent[rb]) parent[rb] = parent[parent[rb]];
        if (ra == rb) continue;
        parent[ra] = rb;
        total += es[i].w;
        {
            JSValue e = JS_NewArray(ctx);
            if (JS_IsException(e)) {
                JS_FreeValue(ctx, res); JS_FreeValue(ctx, edges);
                free(es); free(parent); return JS_EXCEPTION;
            }
            if (JS_DefinePropertyValueUint32(ctx, e, 0, JS_NewInt64(ctx, a), JS_PROP_C_W_E) < 0 ||
                JS_DefinePropertyValueUint32(ctx, e, 1, JS_NewInt64(ctx, b), JS_PROP_C_W_E) < 0 ||
                JS_DefinePropertyValueUint32(ctx, e, 2, JS_NewFloat64(ctx, es[i].w), JS_PROP_C_W_E) < 0 ||
                JS_DefinePropertyValueUint32(ctx, edges, out++, e, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, res); JS_FreeValue(ctx, edges);
                free(es); free(parent); return JS_EXCEPTION;
            }
        }
    }
    free(es); free(parent);
    if (JS_DefinePropertyValueStr(ctx, res, "weight", JS_NewFloat64(ctx, total), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "edges", edges, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, edges);   /* free on the failing branch: the old code
                                      * leaked the whole result array here */
        JS_FreeValue(ctx, res); return JS_EXCEPTION;
    }
    return res;
}

/* aStar(src, dst, heuristic): { dist, path }. heuristic(node)->number is called
 * for EVERY node UP FRONT (before the search), so no JS re-enters the C search
 * loop; a heuristic that mutates the graph is rejected. */
static JSValue dyn_graph_astar(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_graph_t *g;
    uint32_t src, dst;
    JSValueConst heur;
    double *h, *gscore;
    int *prev;
    char *closed;
    uint32_t i, n0;
    uint64_t m0;    /* g->m is uint64; the old uint32 copy falsely reported
                     * "heuristic mutated the graph" past 2^32 edges */
    dyn_heap heap = { NULL, 0, 0 };
    dyn_hp_item it;
    JSValue res, path;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &dst))
        return JS_EXCEPTION;
    heur = argv[2];
    if (!JS_IsFunction(ctx, heur))
        return JS_ThrowTypeError(ctx, "aStar: heuristic must be a function");
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n || dst >= g->n)
        return JS_ThrowRangeError(ctx, "aStar: node out of range");
    /* Refuse negative edges UP FRONT: the old per-relaxation check only fired
     * when the search reached the edge, so the same graph threw from one
     * source pair and succeeded from another. */
    if (graph_has_negative(g))
        return JS_ThrowRangeError(ctx, "aStar: negative edge weight");
    n0 = g->n;
    m0 = g->m;
    /* precompute h[] for all nodes -- the only JS re-entry, and it happens
     * before we touch the adjacency arrays in the search loop */
    h = (double *)malloc((size_t)n0 * sizeof(double));
    if (!h) return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n0; i++) {
        JSValue arg = JS_NewInt64(ctx, i), r;
        r = JS_Call(ctx, heur, JS_UNDEFINED, 1, (JSValueConst *)&arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(r)) { free(h); return JS_EXCEPTION; }
        if (JS_ToFloat64(ctx, &h[i], r)) { JS_FreeValue(ctx, r); free(h); return JS_EXCEPTION; }
        JS_FreeValue(ctx, r);
        /* a NaN f-value breaks the heap's order and every relaxation below
         * it; refuse rather than return a silently wrong path. */
        if (!isfinite(h[i])) { free(h); return JS_ThrowRangeError(ctx,
            "aStar: heuristic must return a finite number"); }
    }
    /* re-resolve: the heuristic may have mutated `this`. n catches nodes and
     * m (monotone: no edge-removal API exists) catches edges added between
     * existing nodes, which the old n-only check let through. */
    g = graph_of(ctx, this_val);
    if (!g) { free(h); return JS_EXCEPTION; }
    if (g->n != n0 || g->m != m0)
        { free(h); return JS_ThrowTypeError(ctx, "aStar: heuristic mutated the graph"); }

    gscore = (double *)malloc((size_t)n0 * sizeof(double));
    prev = (int *)malloc((size_t)n0 * sizeof(int));
    closed = (char *)calloc(n0, 1);
    if (!gscore || !prev || !closed) { free(h); free(gscore); free(prev); free(closed);
                                       return JS_ThrowOutOfMemory(ctx); }
    for (i = 0; i < n0; i++) { gscore[i] = INFINITY; prev[i] = -1; }
    gscore[src] = 0.0;
    if (heap_push(&heap, h[src], src)) { free(h); free(gscore); free(prev); free(closed);
                                         free(heap.a); return JS_ThrowOutOfMemory(ctx); }
    while (heap_pop(&heap, &it)) {
        int u = it.node;
        if (closed[u]) continue;
        closed[u] = 1;
        if (u == dst) break;
        for (i = 0; i < g->adj[u].n; i++) {
            int v = g->adj[u].e[i].to;
            double w = g->adj[u].e[i].w;
            if (gscore[u] + w < gscore[v]) {
                gscore[v] = gscore[u] + w;
                prev[v] = u;
                if (heap_push(&heap, gscore[v] + h[v], v)) {
                    free(h); free(gscore); free(prev); free(closed); free(heap.a);
                    return JS_ThrowOutOfMemory(ctx);
                }
            }
        }
    }
    free(heap.a); free(h); free(closed);
    /* build path by walking prev[] back from dst */
    res = JS_NewObject(ctx);
    path = JS_NewArray(ctx);
    if (JS_IsException(res) || JS_IsException(path)) {
        JS_FreeValue(ctx, res); JS_FreeValue(ctx, path); free(gscore); free(prev);
        return JS_EXCEPTION;
    }
    if (gscore[dst] != INFINITY) {
        int *rev = (int *)malloc((size_t)n0 * sizeof(int));
        uint32_t len = 0, k;
        if (!rev) { JS_FreeValue(ctx, res); JS_FreeValue(ctx, path); free(gscore); free(prev);
                    return JS_ThrowOutOfMemory(ctx); }
        { int cur = dst; while (cur != -1) { rev[len++] = cur; cur = prev[cur]; } }
        for (k = 0; k < len; k++)
            if (JS_DefinePropertyValueUint32(ctx, path, k,
                    JS_NewInt64(ctx, rev[len - 1 - k]), JS_PROP_C_W_E) < 0) {
                free(rev); JS_FreeValue(ctx, res); JS_FreeValue(ctx, path);
                free(gscore); free(prev); return JS_EXCEPTION;
            }
        free(rev);
    }
    {
        double dd = gscore[dst];
        free(gscore); free(prev);
        if (JS_DefinePropertyValueStr(ctx, res, "dist", JS_NewFloat64(ctx, dd), JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, res, "path", path, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, path);   /* the old code leaked the built path */
            JS_FreeValue(ctx, res); return JS_EXCEPTION;
        }
    }
    return res;
}

static const JSCFunctionListEntry dyn_graph_proto[] = {
    JS_CFUNC_DEF("addNode", 0, dyn_graph_add_node),
    JS_CFUNC_DEF("addEdge", 2, dyn_graph_add_edge),
    JS_CFUNC_DEF("neighbors", 1, dyn_graph_neighbors),
    JS_CFUNC_DEF("hasEdge", 2, dyn_graph_has_edge),
    JS_CFUNC_DEF("bfs", 1, dyn_graph_bfs),
    JS_CFUNC_DEF("dfs", 1, dyn_graph_dfs),
    JS_CFUNC_DEF("dijkstra", 2, dyn_graph_dijkstra),
    JS_CFUNC_DEF("bellmanFord", 1, dyn_graph_bellman_ford),
    JS_CFUNC_DEF("topologicalSort", 0, dyn_graph_topo_sort),
    JS_CFUNC_DEF("connectedComponents", 0, dyn_graph_components),
    JS_CFUNC_DEF("floydWarshall", 0, dyn_graph_floyd),
    JS_CFUNC_DEF("mst", 0, dyn_graph_mst),
    JS_CFUNC_DEF("aStar", 3, dyn_graph_astar),
    JS_CGETSET_DEF("nodeCount", dyn_graph_node_count, NULL),
    JS_CGETSET_DEF("edgeCount", dyn_graph_edge_count, NULL),
};

/* ===================================================================== *
 *  serialize()/deserialize() codec
 * ===================================================================== */

/* A node count can never be this, so it distinguishes the varint form from the
 * fixed-width one an older reader wrote. */
#define DYN_GRAPH_EXT 0xFFFFFFFFu

static int dyn_graph_write(JSContext *ctx, dyn_ser_t *w, JSValueConst obj)
{
    dyn_graph_t *g = (dyn_graph_t *)JS_GetOpaque(obj, dyn_graph_class_id);
    uint32_t i, j;

    (void)ctx;
    if (!g)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)(g->directed != 0)) < 0 ||
        dyn_ser_u8(w, (uint8_t)(g->weighted != 0)) < 0 ||
        dyn_ser_u32(w, DYN_GRAPH_EXT) < 0 ||
        dyn_ser_u32(w, g->n) < 0)
        return -1;
    /* Weights are written only for a weighted graph; an unweighted one forces
     * every w to 1.0 anyway, so storing them would be 8 bytes per edge of a
     * constant. */
    /* A degree is small and a target is bounded by n, so both are varints. The
     * target is written as a DELTA from the previous one in the same list --
     * adjacency is often near-sorted, and a zigzag delta is then one byte. */
    for (i = 0; i < g->n; i++) {
        const dyn_adj_t *a = &g->adj[i];
        int64_t prev = 0;
        if (dyn_ser_uvarint(w, a->n) < 0)
            return -1;
        for (j = 0; j < a->n; j++) {
            int64_t to = (int64_t)a->e[j].to;
            if (dyn_ser_svarint(w, to - prev) < 0)
                return -1;
            prev = to;
            if (g->weighted && dyn_ser_f64(w, a->e[j].w) < 0)
                return -1;
        }
    }
    return 0;
}

static JSValue dyn_graph_read(JSContext *ctx, dyn_de_t *r, JSValueConst opts)
{
    dyn_graph_t *g;
    uint32_t n, i, j, self_loops = 0;
    uint64_t deg_total = 0;
    int directed, weighted, varint_form = 0;

    (void)opts;
    directed = dyn_de_u8(r) != 0;
    weighted = dyn_de_u8(r) != 0;
    {
        uint32_t first = dyn_de_u32(r);
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        if (first == DYN_GRAPH_EXT) {
            varint_form = 1;
            /* A varint degree is at least one byte per node. */
            if (dyn_de_count(r, &n, 1) < 0)
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        } else {
            /* The old form: `first` was n, and fixed-width fields follow, so
             * every node costs at least its own u32 degree. */
            n = first;
            if ((uint64_t)n * 4 > (uint64_t)dyn_de_left(r))
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }

    g = (dyn_graph_t *)malloc(sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->adj = NULL;
    g->n = g->cap = 0;
    g->m = 0;
    g->directed = directed;
    g->weighted = weighted;
    if (n) {
        /* calloc, and n published before the fill loop: every early return
         * below goes through dyn_graph_free, which walks adj[0..n-1]. */
        g->adj = (dyn_adj_t *)calloc(n, sizeof(dyn_adj_t));
        if (!g->adj) {
            free(g);
            return JS_ThrowOutOfMemory(ctx);
        }
        g->n = g->cap = n;
    }
    for (i = 0; i < n; i++) {
        uint32_t deg;
        int64_t prev = 0;
        if (varint_form) {
            uint64_t d;
            /* A varint edge is at least one byte, plus 8 for a weight, so a
             * forged degree still cannot outrun what remains. The u64
             * multiply itself can wrap for d >= 2^64/9, which defeats the
             * guard AND truncates below -- refuse the absurd count first. */
            if (dyn_de_uvarint(r, &d) < 0 || d > UINT32_MAX ||
                d * (weighted ? 9u : 1u) > (uint64_t)dyn_de_left(r)) {
                dyn_graph_free(g);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            deg = (uint32_t)d;
        } else if (dyn_de_count(r, &deg, weighted ? 12 : 4) < 0) {
            dyn_graph_free(g);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (deg) {
            g->adj[i].e = (dyn_edge_t *)malloc((size_t)deg * sizeof(dyn_edge_t));
            if (!g->adj[i].e) {
                dyn_graph_free(g);
                return JS_ThrowOutOfMemory(ctx);
            }
            g->adj[i].cap = deg;
        }
        for (j = 0; j < deg; j++) {
            uint32_t to;
            double wt;
            if (varint_form) {
                int64_t d;
                if (dyn_de_svarint(r, &d) < 0 || prev + d < 0) {
                    dyn_graph_free(g);
                    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
                }
                prev += d;
                to = (uint32_t)prev;
            } else {
                to = dyn_de_u32(r);
            }
            wt = weighted ? dyn_de_f64(r) : 1.0;
            if (!isfinite(wt)) {
                dyn_graph_free(g);
                return JS_ThrowTypeError(ctx,
                    "malformed Graph record: non-finite edge weight");
            }
            if (to >= n) {
                dyn_graph_free(g);
                return JS_ThrowTypeError(ctx,
                    "malformed Graph record: edge to node %u, node count %u",
                    to, n);
            }
            g->adj[i].e[j].to = (int)to;
            g->adj[i].e[j].w = wt;
            g->adj[i].n++;
            if (to == i)
                self_loops++;
        }
        deg_total += deg;
    }
    if (!dyn_de_ok(r)) {
        dyn_graph_free(g);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    /* edgeCount counts addEdge calls, not adjacency entries: a directed edge
     * pushes one entry, an undirected one pushes two -- except a self-loop,
     * which pushes one either way. Recomputed, so a record cannot forge it. */
    if (directed) {
        g->m = deg_total;
    } else {
        if (((deg_total + self_loops) & 1u) != 0) {
            dyn_graph_free(g);
            return JS_ThrowTypeError(ctx,
                "malformed Graph record: asymmetric undirected adjacency");
        }
        g->m = (deg_total + self_loops) / 2;
    }
    return dyn_plain_wrap(ctx, dyn_graph_class_id, g, dyn_graph_free);
}

static const dyn_codec_t dyn_graph_codec = {
    0, DYN_TID_GRAPH, "Graph", dyn_graph_write, dyn_graph_read,
};

int dyn_graph_register(JSContext *ctx, JSModuleDef *m)
{
    dyn_codec_t c = dyn_graph_codec;

    if (dyn_register_plain_class(ctx, m, &dyn_graph_class_id, &dyn_graph_class,
                                 dyn_graph_proto, countof(dyn_graph_proto),
                                 dyn_graph_ctor, "Graph") < 0)
        return -1;
    /* Registered here, where the class id has just been assigned. */
    c.class_id = dyn_graph_class_id;
    return dyn_codec_register(&c);
}

void dyn_graph_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "Graph");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_STRUCTURES */
