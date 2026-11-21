#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

JSValue dyn_graph_add_edge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t u, v;
    double w = 1.0;
    uint8_t t = 0;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &v))
        return JS_EXCEPTION;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &w, argv[2]))
        return JS_EXCEPTION;
    if (!isfinite(w))
        return JS_ThrowRangeError(ctx, "edge weight must be finite");
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && dyn_graph_type_arg(ctx, argv[3], "Graph.addEdge", &t))
        return JS_EXCEPTION;
    if (u >= DYN_GRAPH_MAX_NODES || v >= DYN_GRAPH_MAX_NODES)
        return JS_ThrowRangeError(ctx, "Graph.addEdge: node id %u is past the 2^26-node limit", u > v ? u : v);
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (dyn_gk_add_edge_typed(g, u, v, w, t) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_graph_neighbors(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
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
                JS_NewInt64(ctx, g->adj[u].e[i].to), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

JSValue dyn_graph_has_edge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t u, v;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    if (dyn_idx_arg(ctx, argv[1], &v))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, dyn_gk_has_edge(g, u, v));
}

JSValue dyn_graph_edge_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_graph_t* g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->m);
}

static int dyn_graph_get_i32(JSContext* ctx, JSValueConst v, int32_t** pp,
    uint32_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    if (JS_GetClassID(v) != dyn_graph_i32_class_id) {
        JS_ThrowTypeError(ctx, "dyna:graph expected an Int32Array");
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 4) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:graph expected an Int32Array");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:graph Int32Array out of bounds");
        return -1;
    }
    *pp = (int32_t*)(base + off);
    *pn = (uint32_t)(len / 4);
    return 0;
}

JSValue dyn_graph_neighbors_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t u, n, i;
    int32_t* out;
    int64_t deg;

    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &u))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (u >= g->n)
        return JS_ThrowRangeError(ctx, "neighborsInto: node out of range");
    if (JS_CheckWritableBuffer(ctx, argv[1]) < 0 || dyn_graph_get_i32(ctx, argv[1], &out, &n))
        return JS_EXCEPTION;
    deg = (int64_t)g->adj[u].n;
    if ((uint64_t)deg > (uint64_t)n)
        return JS_ThrowRangeError(ctx,
            "neighborsInto: out has %u slots, node %u has %lld neighbors",
            n, u, (long long)deg);
    for (i = 0; i < g->adj[u].n; i++)
        out[i] = g->adj[u].e[i].to;
    return JS_NewInt64(ctx, deg);
}

#endif
