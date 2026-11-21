#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

static JSValue int_array(JSContext* ctx, const int* a, uint32_t n)
{
    JSValue* v;
    JSValue arr;
    uint32_t i;
    if (n > (uint32_t)INT32_MAX)
        return JS_ThrowRangeError(ctx, "invalid array length");
    v = (JSValue*)malloc((n ? n : 1) * sizeof(JSValue));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++)
        v[i] = JS_NewInt32(ctx, a[i]);
    arr = JS_NewArrayFrom(ctx, (int)n, v);
    free(v);
    return arr;
}

static int dyn_graph_visit_opt(JSContext* ctx, JSValueConst opts,
    const char* op, JSValue* pvisit)
{
    static const char* const keys[] = { "visit" };

    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: expected an options object with a visit "
                               "callback",
            op);
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, keys, 1))
        return -1;
    *pvisit = JS_GetPropertyStr(ctx, opts, "visit");
    if (JS_IsException(*pvisit))
        return -1;
    if (!JS_IsFunction(ctx, *pvisit)) {
        JS_FreeValue(ctx, *pvisit);
        *pvisit = JS_UNDEFINED;
        JS_ThrowTypeError(ctx, "%s: options.visit must be a function", op);
        return -1;
    }
    return 0;
}

typedef struct {
    JSContext* ctx;
    JSValueConst visit;
    JSValueConst self;
    uint32_t n0;
    uint64_t m0;
} dyn_graph_vis_t;

static int dyn_graph_emit(void* ud, int node)
{
    dyn_graph_vis_t* vs = (dyn_graph_vis_t*)ud;
    dyn_graph_t* g;
    JSValue arg, r;
    int stop;

    arg = JS_NewInt64(vs->ctx, node);
    r = JS_Call(vs->ctx, vs->visit, JS_UNDEFINED, 1, (JSValueConst*)&arg);
    JS_FreeValue(vs->ctx, arg);
    if (JS_IsException(r))
        return -1;
    g = graph_of(vs->ctx, vs->self);
    if (!g) {
        JS_FreeValue(vs->ctx, r);
        return -1;
    }
    if (g->n != vs->n0 || g->m != vs->m0) {
        JS_FreeValue(vs->ctx, r);
        return DYN_GK_E_MUTATED;
    }
    stop = JS_ToBool(vs->ctx, r);
    JS_FreeValue(vs->ctx, r);
    return stop < 0 ? -1 : !stop;
}

static int dyn_graph_walk_csr(JSContext* ctx, dyn_graph_t* g, dyn_gk_csr_t* c, const char* op)
{
    int rc = dyn_gk_csr_of_graph(g, 0, c);
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

static JSValue dyn_graph_walk_throw(JSContext* ctx, int rc, const char* op, const char* too_large)
{
    if (rc == DYN_GK_E_OOM)
        return JS_ThrowOutOfMemory(ctx);
    if (rc == DYN_GK_E_MUTATED)
        return JS_ThrowTypeError(ctx, "%s: the visitor mutated the graph", op);
    if (rc == DYN_GK_E_TOO_LARGE)
        return JS_ThrowRangeError(ctx, "%s", too_large);
    if (rc == DYN_GK_E_CYCLE)
        return JS_ThrowRangeError(ctx, "%s: graph has a cycle", op);
    return JS_EXCEPTION;
}

static JSValue dyn_graph_visit(JSContext* ctx, JSValueConst self, dyn_graph_t* g, uint32_t src, JSValue visit, int kind, int has, uint32_t mask)
{
    static const char* const ops[] = { "bfs", "dfs", "topologicalSort" };
    dyn_graph_vis_t vs;
    dyn_gk_csr_t c;
    uint32_t visited = 0;
    int rc;
    if (dyn_graph_csr_for(ctx, g, 0, has, mask, &c, ops[kind])) {
        JS_FreeValue(ctx, visit);
        return JS_EXCEPTION;
    }
    vs.ctx = ctx;
    vs.visit = visit;
    vs.self = self;
    vs.n0 = g->n;
    vs.m0 = g->m;
    if (kind == 0)
        rc = dyn_gk_bfs_visit(&c, src, dyn_graph_emit, &vs, &visited);
    else if (kind == 1)
        rc = dyn_gk_dfs_visit(&c, src, dyn_graph_emit, &vs, &visited);
    else
        rc = dyn_gk_topo_visit(&c, dyn_graph_emit, &vs, &visited);
    dyn_gk_csr_free(&c);
    JS_FreeValue(ctx, visit);
    if (rc < 0)
        return dyn_graph_walk_throw(ctx, rc, ops[kind], "dfs: graph too large to traverse");
    return JS_NewInt64(ctx, visited);
}

static int dyn_graph_walk_opts(JSContext* ctx, JSValueConst opts, const char* op, JSValue* pvisit, uint32_t* mask, int* has)
{
    static const char* const keys[] = { "visit", "types" };
    JSValue t;
    *pvisit = JS_UNDEFINED;
    *has = 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object with visit and/or types", op);
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, keys, 2))
        return -1;
    *pvisit = JS_GetPropertyStr(ctx, opts, "visit");
    if (JS_IsException(*pvisit))
        return -1;
    if (!JS_IsUndefined(*pvisit) && !JS_IsFunction(ctx, *pvisit)) {
        JS_FreeValue(ctx, *pvisit);
        *pvisit = JS_UNDEFINED;
        JS_ThrowTypeError(ctx, "%s: options.visit must be a function", op);
        return -1;
    }
    t = JS_GetPropertyStr(ctx, opts, "types");
    if (JS_IsException(t) || (!JS_IsUndefined(t) && dyn_graph_mask_arg(ctx, t, op, mask))) {
        JS_FreeValue(ctx, t);
        JS_FreeValue(ctx, *pvisit);
        *pvisit = JS_UNDEFINED;
        return -1;
    }
    *has = !JS_IsUndefined(t);
    JS_FreeValue(ctx, t);
    if (!*has && JS_IsUndefined(*pvisit)) {
        JS_ThrowTypeError(ctx, "%s: options must set visit and/or types", op);
        return -1;
    }
    return 0;
}

static JSValue dyn_graph_order(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int kind)
{
    dyn_graph_t* g;
    uint32_t src, mask = DYN_GRAPH_ALL_TYPES;
    int* order;
    int64_t no;
    int has = 0;
    JSValue res, visit = JS_UNDEFINED;

    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])
        && dyn_graph_walk_opts(ctx, argv[1], kind ? "dfs" : "bfs", &visit, &mask, &has))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g || src >= g->n) {
        JS_FreeValue(ctx, visit);
        return g ? JS_ThrowRangeError(ctx, "source out of range") : JS_EXCEPTION;
    }
    if (!JS_IsUndefined(visit))
        return dyn_graph_visit(ctx, this_val, g, src, visit, kind, has, mask);
    order = (int*)malloc((size_t)g->n * sizeof(int));
    no = !order ? DYN_GK_E_OOM : has ? (kind == 0 ? dyn_gk_bfs_mask(g, src, mask, order) : dyn_gk_dfs_mask(g, src, mask, order))
                               : kind == 0 ? dyn_gk_bfs_adj(g, src, order)
                                           : dyn_gk_dfs_adj(g, src, order);
    if (no < 0) {
        free(order);
        return dyn_graph_walk_throw(ctx, (int)no, "dfs", "graph too large to traverse");
    }
    res = int_array(ctx, order, (uint32_t)no);
    free(order);
    return res;
}

JSValue dyn_graph_bfs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_graph_order(ctx, this_val, argc, argv, 0);
}

JSValue dyn_graph_dfs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_graph_order(ctx, this_val, argc, argv, 1);
}

JSValue dyn_graph_topo_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    int* order;
    int64_t no;
    dyn_gk_csr_t c;
    JSValue res;

    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!g->directed)
        return JS_ThrowTypeError(ctx, "topologicalSort requires a directed graph");
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue visit;
        if (dyn_graph_visit_opt(ctx, argv[0], "topologicalSort", &visit))
            return JS_EXCEPTION;
        return dyn_graph_visit(ctx, this_val, g, 0, visit, 2, 0, DYN_GRAPH_ALL_TYPES);
    }
    if (dyn_graph_walk_csr(ctx, g, &c, "topologicalSort"))
        return JS_EXCEPTION;
    order = (int*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    no = order ? dyn_gk_topo(&c, order) : DYN_GK_E_OOM;
    dyn_gk_csr_free(&c);
    if (no < 0) {
        free(order);
        return dyn_graph_walk_throw(ctx, (int)no, "topologicalSort", "");
    }
    res = int_array(ctx, order, (uint32_t)no);
    free(order);
    return res;
}

JSValue dyn_graph_components(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    int* comp;
    int rc;
    dyn_gk_csr_t c;
    JSValue res;
    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (dyn_graph_walk_csr(ctx, g, &c, "connectedComponents"))
        return JS_EXCEPTION;
    comp = (int*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int));
    rc = comp ? dyn_gk_components(&c, comp) : DYN_GK_E_OOM;
    dyn_gk_csr_free(&c);
    if (rc < 0) {
        free(comp);
        return dyn_graph_walk_throw(ctx, rc, "connectedComponents", "graph.connectedComponents: too many incidences");
    }
    res = int_array(ctx, comp, g->n);
    free(comp);
    return res;
}

#endif
