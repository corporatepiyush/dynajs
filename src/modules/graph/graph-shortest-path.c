#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

static JSValue dbl_array(JSContext* ctx, const double* a, uint32_t n)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewFloat64(ctx, a[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static int gsp_csr(JSContext* ctx, dyn_graph_t* g, int with_w, dyn_gk_csr_t* c, const char* op)
{
    int rc = dyn_gk_csr_of_graph(g, with_w, c);
    if (rc == DYN_GK_E_TOO_LARGE) {
        JS_ThrowRangeError(ctx, "%s: graph too large (more than 2^32-1 arcs)", op);
        return -1;
    }
    if (rc < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue gsp_typed(JSContext* ctx, void* p, size_t bytes, JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;
    if (!p) {
        p = malloc(1);
        if (!p)
            return JS_ThrowOutOfMemory(ctx);
    }
    ab = dyn_nat_ab_take(ctx, p, bytes);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue gsp_path_array(JSContext* ctx, const int* path, uint32_t len)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t k;
    if (JS_IsException(arr))
        return arr;
    for (k = 0; k < len; k++)
        if (JS_DefinePropertyValueUint32(ctx, arr, k, JS_NewInt64(ctx, path[k]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static JSValue gsp_obj(JSContext* ctx, const char* const* keys, JSValue* v, int n)
{
    JSValue o = JS_NewObject(ctx);
    int i, bad = JS_IsException(o);
    for (i = 0; i < n; i++) {
        if (bad || JS_IsException(v[i])) {
            bad = 1;
            JS_FreeValue(ctx, v[i]);
            continue;
        }
        if (JS_DefinePropertyValueStr(ctx, o, keys[i], v[i], JS_PROP_C_W_E) < 0)
            bad = 1;
    }
    if (bad) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue gsp_throw(JSContext* ctx, int rc, const char* op)
{
    if (rc == DYN_GK_E_NEG_WEIGHT)
        return JS_ThrowRangeError(ctx, "%s: negative edge weight", op);
    if (rc == DYN_GK_E_CYCLE)
        return JS_ThrowRangeError(ctx, "%s: graph has a cycle", op);
    if (rc == DYN_GK_E_PRED_CYCLE)
        return JS_ThrowInternalError(ctx, "graph.%s: predecessor cycle in the reconstructed path", op);
    return JS_ThrowOutOfMemory(ctx);
}

static int gsp_src(JSContext* ctx, JSValueConst this_val, JSValueConst a, uint32_t* src, dyn_graph_t** pg)
{
    if (dyn_idx_arg(ctx, a, src))
        return -1;
    *pg = graph_of(ctx, this_val);
    if (!*pg)
        return -1;
    if (*src >= (*pg)->n) {
        JS_ThrowRangeError(ctx, "source out of range");
        return -1;
    }
    return 0;
}

JSValue dyn_graph_shortest_path(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    dyn_gk_csr_t c;
    uint32_t src, dst, len = 0;
    double* dist;
    int32_t* pred;
    int* path = NULL;
    int rc;
    double d;
    JSValue v[2];
    static const char* const keys[] = { "dist", "path" };
    uint32_t mask = DYN_GRAPH_ALL_TYPES;
    int has;
    if (dyn_graph_types_opt(ctx, argc >= 3 ? argv[2] : JS_UNDEFINED, "shortestPath", &mask, &has))
        return JS_EXCEPTION;
    if (gsp_src(ctx, this_val, argv[0], &src, &g) || dyn_idx_arg(ctx, argv[1], &dst))
        return JS_EXCEPTION;
    if (dst >= g->n)
        return JS_ThrowRangeError(ctx, "destination out of range");
    if (dyn_graph_csr_for(ctx, g, g->weighted, has, mask, &c, "shortestPath"))
        return JS_EXCEPTION;
    dist = (double*)malloc((size_t)g->n * sizeof(double));
    pred = (int32_t*)malloc((size_t)g->n * sizeof(int32_t));
    rc = dist && pred ? dyn_gk_dijkstra(&c, src, dist, pred) : DYN_GK_E_OOM;
    d = rc == 0 ? dist[dst] : 0.0;
    if (rc == 0 && d != DYN_INFINITY)
        rc = dyn_gk_pred_path(pred, c.n, dst, &path, &len);
    dyn_gk_csr_free(&c);
    free(dist);
    free(pred);
    if (rc < 0)
        return gsp_throw(ctx, rc, "shortestPath");
    v[0] = JS_NewFloat64(ctx, d);
    v[1] = gsp_path_array(ctx, path, len);
    free(path);
    return gsp_obj(ctx, keys, v, 2);
}

JSValue dyn_graph_shortest_path_tree(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    dyn_gk_csr_t c;
    uint32_t src, n;
    double* dist;
    int32_t* pred;
    int rc;
    JSValue v[2];
    static const char* const keys[] = { "dist", "pred" };
    (void)argc;
    if (gsp_src(ctx, this_val, argv[0], &src, &g))
        return JS_EXCEPTION;
    if (gsp_csr(ctx, g, g->weighted, &c, "shortestPathTree"))
        return JS_EXCEPTION;
    n = c.n;
    dist = (double*)malloc((size_t)n * sizeof(double));
    pred = (int32_t*)malloc((size_t)n * sizeof(int32_t));
    rc = dist && pred ? dyn_gk_dijkstra(&c, src, dist, pred) : DYN_GK_E_OOM;
    dyn_gk_csr_free(&c);
    if (rc < 0) {
        free(dist);
        free(pred);
        return gsp_throw(ctx, rc, "shortestPathTree");
    }
    v[0] = gsp_typed(ctx, dist, (size_t)n * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    v[1] = gsp_typed(ctx, pred, (size_t)n * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    return gsp_obj(ctx, keys, v, 2);
}

JSValue dyn_graph_critical_path(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    dyn_gk_csr_t c;
    uint32_t n, end = 0, len = 0;
    double* dist;
    int32_t* pred;
    int* path = NULL;
    int rc;
    JSValue v[3];
    static const char* const keys[] = { "length", "path", "dist" };
    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!g->directed)
        return JS_ThrowTypeError(ctx, "criticalPath requires a directed graph");
    if (gsp_csr(ctx, g, g->weighted, &c, "criticalPath"))
        return JS_EXCEPTION;
    n = c.n;
    dist = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
    pred = (int32_t*)malloc((size_t)(n ? n : 1) * sizeof(int32_t));
    rc = dist && pred ? dyn_gk_dag_longest(&c, dist, pred, &end) : DYN_GK_E_OOM;
    if (rc == 0 && n)
        rc = dyn_gk_pred_path(pred, n, end, &path, &len);
    dyn_gk_csr_free(&c);
    free(pred);
    if (rc < 0) {
        free(dist);
        return gsp_throw(ctx, rc, "criticalPath");
    }
    v[0] = JS_NewFloat64(ctx, n ? dist[end] : 0.0);
    v[1] = gsp_path_array(ctx, path, len);
    free(path);
    v[2] = gsp_typed(ctx, dist, (size_t)n * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    return gsp_obj(ctx, keys, v, 3);
}

JSValue dyn_graph_dijkstra(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t src, dst = 0;
    int have_dst = 0, rc;
    double* dist;
    dyn_gk_csr_t c;
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
    if (gsp_csr(ctx, g, g->weighted, &c, "dijkstra"))
        return JS_EXCEPTION;
    dist = (double*)malloc((size_t)g->n * sizeof(double));
    rc = dist ? dyn_gk_dijkstra(&c, src, dist, NULL) : DYN_GK_E_OOM;
    dyn_gk_csr_free(&c);
    if (rc < 0) {
        free(dist);
        if (rc == DYN_GK_E_NEG_WEIGHT)
            return JS_ThrowRangeError(ctx, "dijkstra: negative edge weight");
        return JS_ThrowOutOfMemory(ctx);
    }
    if (have_dst) {
        double d = dist[dst];
        free(dist);
        return JS_NewFloat64(ctx, d);
    }
    res = dbl_array(ctx, dist, g->n);
    free(dist);
    return res;
}

JSValue dyn_graph_bellman_ford(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t src;
    double* dist;
    dyn_gk_csr_t c;
    int rc;
    JSValue res;
    (void)argc;
    if (gsp_src(ctx, this_val, argv[0], &src, &g))
        return JS_EXCEPTION;
    if (gsp_csr(ctx, g, g->weighted, &c, "bellmanFord"))
        return JS_EXCEPTION;
    dist = (double*)malloc((size_t)g->n * sizeof(double));
    rc = dist ? dyn_gk_bellman_ford(&c, src, dist) : DYN_GK_E_OOM;
    dyn_gk_csr_free(&c);
    if (rc == DYN_GK_E_OOM) {
        free(dist);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (rc < 0) {
        free(dist);
        return JS_ThrowRangeError(ctx, "bellmanFord: negative cycle");
    }
    res = dbl_array(ctx, dist, g->n);
    free(dist);
    return res;
}

JSValue dyn_graph_floyd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    double* d;
    uint32_t n, i;
    int rc;
    JSValue res;
    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    n = g->n;
    if (n > DYN_FLOYD_MAX_N)
        rc = DYN_GK_E_TOO_LARGE;
    else {
        dyn_gk_csr_t c;
        if (gsp_csr(ctx, g, g->weighted, &c, "floydWarshall"))
            return JS_EXCEPTION;
        rc = dyn_gk_floyd(&c, &d);
        dyn_gk_csr_free(&c);
    }
    if (rc == DYN_GK_E_TOO_LARGE)
        return JS_ThrowRangeError(ctx,
            "floydWarshall: graph too large (n > %u); the algorithm is O(n^3)",
            (unsigned)DYN_FLOYD_MAX_N);
    if (rc == DYN_GK_E_NEG_CYCLE)
        return JS_ThrowRangeError(ctx, "floydWarshall: negative cycle");
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    res = JS_NewArray(ctx);
    if (JS_IsException(res)) {
        free(d);
        return res;
    }
    for (i = 0; i < n; i++) {
        JSValue row = dbl_array(ctx, d + (size_t)i * n, n);
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, res);
            free(d);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, res, i, row, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, row);
            JS_FreeValue(ctx, res);
            free(d);
            return JS_EXCEPTION;
        }
    }
    free(d);
    return res;
}

static int dyn_graph_heuristic(JSContext* ctx, JSValueConst heur, double* h, uint32_t n0)
{
    uint32_t i;
    for (i = 0; i < n0; i++) {
        JSValue arg = JS_NewInt64(ctx, i), r;
        r = JS_Call(ctx, heur, JS_UNDEFINED, 1, (JSValueConst*)&arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(r))
            return -1;
        if (JS_ToFloat64(ctx, &h[i], r)) {
            JS_FreeValue(ctx, r);
            return -1;
        }
        JS_FreeValue(ctx, r);
        if (!isfinite(h[i])) {
            JS_ThrowRangeError(ctx, "aStar: heuristic must return a finite number");
            return -1;
        }
    }
    return 0;
}

JSValue dyn_graph_astar(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t src, dst, n0, len = 0, k;
    uint64_t m0;
    JSValueConst heur;
    double* h;
    double dd = DYN_INFINITY;
    int* rev = NULL;
    int rc;
    dyn_gk_csr_t c;
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
    if (gsp_csr(ctx, g, g->weighted, &c, "aStar"))
        return JS_EXCEPTION;
    if (dyn_gk_has_negative_w(&c, 1)) {
        dyn_gk_csr_free(&c);
        return JS_ThrowRangeError(ctx, "aStar: negative edge weight");
    }
    n0 = g->n;
    m0 = g->m;
    h = (double*)malloc((size_t)n0 * sizeof(double));
    if (!h) {
        dyn_gk_csr_free(&c);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_graph_heuristic(ctx, heur, h, n0) < 0) {
        dyn_gk_csr_free(&c);
        free(h);
        return JS_EXCEPTION;
    }
    g = graph_of(ctx, this_val);
    if (!g) {
        dyn_gk_csr_free(&c);
        free(h);
        return JS_EXCEPTION;
    }
    if (g->n != n0 || g->m != m0) {
        dyn_gk_csr_free(&c);
        free(h);
        return JS_ThrowTypeError(ctx, "aStar: heuristic mutated the graph");
    }
    rc = dyn_gk_astar(&c, src, dst, h, &dd, &rev, &len);
    dyn_gk_csr_free(&c);
    free(h);
    if (rc == DYN_GK_E_BUDGET)
        return JS_ThrowRangeError(ctx,
            "aStar: heuristic reopens too many nodes "
            "(push budget exceeded)");
    if (rc == DYN_GK_E_PRED_CYCLE)
        return JS_ThrowInternalError(ctx,
            "graph.aStar: predecessor cycle in the reconstructed path");
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    res = JS_NewObject(ctx);
    path = JS_NewArray(ctx);
    if (JS_IsException(res) || JS_IsException(path))
        goto fail;
    for (k = 0; k < len; k++)
        if (JS_DefinePropertyValueUint32(ctx, path, k, JS_NewInt64(ctx, rev[k]), JS_PROP_C_W_E) < 0)
            goto fail;
    free(rev);
    if (JS_DefinePropertyValueStr(ctx, res, "dist", JS_NewFloat64(ctx, dd), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "path", path, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, path);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
fail:
    free(rev);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, path);
    return JS_EXCEPTION;
}

#endif
