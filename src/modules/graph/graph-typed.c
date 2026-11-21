#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

#define DYN_GRAPH_MAX_HOPS 1024u
#define DYN_GRAPH_PATH_BUDGET (1000ULL * 1000ULL * 1000ULL)

int dyn_graph_mask_arg(JSContext* ctx, JSValueConst v, const char* op, uint32_t* mask)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "%s: a type mask must be a number", op);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= -2147483648.0 && d <= 4294967295.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s: a type mask must be an integer in [-2^31, 2^32-1]", op);
        return -1;
    }
    *mask = (uint32_t)(int64_t)d;
    return 0;
}

int dyn_graph_type_arg(JSContext* ctx, JSValueConst v, const char* op, uint8_t* t)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "%s: an edge type must be a number", op);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0 && d < (double)DYN_GRAPH_MAX_TYPES) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s: an edge type must be an integer in [0, 31]", op);
        return -1;
    }
    *t = (uint8_t)d;
    return 0;
}

int dyn_graph_types_opt(JSContext* ctx, JSValueConst opts, const char* op, uint32_t* mask, int* has)
{
    static const char* const keys[] = { "types" };
    JSValue v;
    int rc;
    *has = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", op);
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, keys, 1))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "types");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = dyn_graph_mask_arg(ctx, v, op, mask);
    JS_FreeValue(ctx, v);
    if (rc)
        return -1;
    *has = 1;
    return 0;
}

int dyn_graph_csr_for(JSContext* ctx, const dyn_graph_t* g, int with_w, int has, uint32_t mask, dyn_gk_csr_t* c, const char* op)
{
    int rc = has ? dyn_gk_csr_of_graph_mask(g, with_w, mask, c) : dyn_gk_csr_of_graph(g, with_w, c);
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

static JSValue dyn_graph_take(JSContext* ctx, void* p, size_t bytes, JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;
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

JSValue dyn_graph_types_array(JSContext* ctx, const dyn_graph_t* g, size_t total)
{
    uint8_t* t = (uint8_t*)malloc(total ? total : 1);
    size_t at = 0;
    uint32_t u, i;
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            t[at++] = g->adj[u].e[i].t;
    return dyn_graph_take(ctx, t, total, JS_TYPED_ARRAY_UINT8);
}

static int dyn_graph_view(JSContext* ctx, JSValueConst v, int type, const char* op, const char* what, uint8_t** p, size_t* len)
{
    static const char* const names[] = { [JS_TYPED_ARRAY_UINT8] = "a Uint8Array", [JS_TYPED_ARRAY_INT32] = "an Int32Array", [JS_TYPED_ARRAY_FLOAT64] = "a Float64Array" };
    JSValue buf = JS_UNDEFINED;
    double off = 0, bl = 0;
    int vt = -1, r;
    size_t size = 0;
    uint8_t* base;
    r = JS_GetViewInfo(ctx, v, &buf, &off, &bl, &vt);
    if (r < 0)
        return -1;
    if (r == 0 || vt != type) {
        if (r > 0)
            JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "%s: %s must be %s", op, what, names[type]);
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &size, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s: %s is detached", op, what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "%s: %s is out of bounds of its buffer", op, what);
        return -1;
    }
    JS_FreeValue(ctx, buf);
    *p = base + (size_t)off;
    *len = (size_t)bl;
    return 0;
}

static int dyn_graph_list(JSContext* ctx, JSValueConst v, const char* op, const char* what, int masks, uint32_t limit, uint32_t** out, uint32_t* n)
{
    JSValue lv;
    double len;
    uint32_t i, *a;
    *out = NULL;
    *n = 0;
    if (!masks && JS_IsNumber(v)) {
        a = (uint32_t*)malloc(sizeof(uint32_t));
        if (!a) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_idx_arg(ctx, v, a)) {
            free(a);
            return -1;
        }
        *out = a;
        *n = 1;
        return 0;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "%s: %s must be %s", op, what, masks ? "an array of type masks" : "a node id or an array of node ids");
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lv) || JS_ToFloat64(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (!(len >= 0))
        len = 0;
    len = floor(len);
    if (len > (double)limit) {
        JS_ThrowRangeError(ctx, "%s: %s holds %.0f entries (at most %u)", op, what, len, limit);
        return -1;
    }
    a = (uint32_t*)malloc(((size_t)len ? (size_t)len : 1) * sizeof(uint32_t));
    if (!a) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < (uint32_t)len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        int rc;
        if (JS_IsException(e)) {
            free(a);
            return -1;
        }
        if (masks)
            rc = dyn_graph_mask_arg(ctx, e, op, &a[i]);
        else if (!JS_IsNumber(e)) {
            JS_ThrowTypeError(ctx, "%s: %s[%u] must be a node id", op, what, i);
            rc = -1;
        } else
            rc = dyn_idx_arg(ctx, e, &a[i]);
        JS_FreeValue(ctx, e);
        if (rc) {
            free(a);
            return -1;
        }
    }
    *out = a;
    *n = (uint32_t)len;
    return 0;
}

static int dyn_graph_check_nodes(JSContext* ctx, const dyn_graph_t* g, const uint32_t* s, uint32_t ns, const char* op)
{
    uint32_t i;
    for (i = 0; i < ns; i++)
        if (s[i] >= g->n) {
            JS_ThrowRangeError(ctx, "%s: source node %u out of range (%u nodes)", op, s[i], g->n);
            return -1;
        }
    return 0;
}

JSValue dyn_graph_neighborhood(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "nodes", "dist" };
    dyn_graph_t* g;
    uint32_t src, k, mask = DYN_GRAPH_ALL_TYPES;
    int has;
    double kd;
    int32_t *order, *dist;
    int64_t cnt;
    JSValue res, a, b;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &src))
        return JS_EXCEPTION;
    if (!JS_IsNumber(argv[1]))
        return JS_ThrowTypeError(ctx, "neighborhood: k must be a number");
    if (JS_ToFloat64(ctx, &kd, argv[1]))
        return JS_EXCEPTION;
    if (!(kd >= 0 && kd <= 4294967295.0) || kd != floor(kd))
        return JS_ThrowRangeError(ctx, "neighborhood: k must be an integer in [0, 2^32-1]");
    k = (uint32_t)kd;
    if (dyn_graph_types_opt(ctx, argv[2], "neighborhood", &mask, &has))
        return JS_EXCEPTION;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (src >= g->n)
        return JS_ThrowRangeError(ctx, "source out of range");
    order = (int32_t*)malloc((size_t)g->n * sizeof(int32_t));
    dist = (int32_t*)malloc((size_t)g->n * sizeof(int32_t));
    cnt = order && dist ? dyn_gk_khop(g, src, k, mask, order, dist) : DYN_GK_E_OOM;
    if (cnt < 0) {
        free(order);
        free(dist);
        return JS_ThrowOutOfMemory(ctx);
    }
    a = dyn_graph_take(ctx, order, (size_t)cnt * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    b = dyn_graph_take(ctx, dist, (size_t)cnt * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    res = JS_NewObject(ctx);
    if (JS_IsException(a) || JS_IsException(b) || JS_IsException(res)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, keys[0], a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, keys[1], b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static int dyn_graph_pattern(JSContext* ctx, JSValueConst this_val, JSValueConst sv, JSValueConst hv, const char* op, dyn_graph_t** pg, uint32_t** src, uint32_t* ns, uint32_t** masks, uint32_t* nh)
{
    *src = *masks = NULL;
    if (!graph_of(ctx, this_val))
        return -1;
    if (dyn_graph_list(ctx, sv, op, "sources", 0, DYN_GRAPH_MAX_NODES, src, ns))
        return -1;
    if (dyn_graph_list(ctx, hv, op, "hops", 1, DYN_GRAPH_MAX_HOPS, masks, nh)) {
        free(*src);
        *src = NULL;
        return -1;
    }
    *pg = graph_of(ctx, this_val);
    if (!*pg || dyn_graph_check_nodes(ctx, *pg, *src, *ns, op)) {
        free(*src);
        free(*masks);
        *src = *masks = NULL;
        return -1;
    }
    return 0;
}

JSValue dyn_graph_path_endpoints(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t *src, *masks, ns, nh;
    int32_t* out;
    int64_t cnt;
    (void)argc;
    if (dyn_graph_pattern(ctx, this_val, argv[0], argv[1], "pathEndpoints", &g, &src, &ns, &masks, &nh))
        return JS_EXCEPTION;
    out = (int32_t*)malloc((size_t)(g->n ? g->n : 1) * sizeof(int32_t));
    cnt = out ? dyn_gk_path_ends(g, src, ns, masks, nh, out) : DYN_GK_E_OOM;
    free(src);
    free(masks);
    if (cnt < 0) {
        free(out);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_graph_take(ctx, out, (size_t)cnt * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
}

static int dyn_graph_overlap(const uint8_t* a, size_t al, const uint8_t* b, size_t bl)
{
    return al && bl && a < b + bl && b < a + al;
}

JSValue dyn_graph_paths_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    uint32_t *src, *masks, ns, nh;
    uint8_t *pn, *pt = NULL;
    size_t ln, lt = 0;
    uint64_t rows, total = 0;
    int rc, with_t = argc >= 4 && !JS_IsUndefined(argv[3]);
    if (dyn_graph_pattern(ctx, this_val, argv[0], argv[1], "pathsInto", &g, &src, &ns, &masks, &nh))
        return JS_EXCEPTION;
    if (dyn_graph_view(ctx, argv[2], JS_TYPED_ARRAY_INT32, "pathsInto", "outNodes", &pn, &ln)
        || JS_CheckWritableBuffer(ctx, argv[2]) < 0
        || (with_t && (dyn_graph_view(ctx, argv[3], JS_TYPED_ARRAY_UINT8, "pathsInto", "outTypes", &pt, &lt) || JS_CheckWritableBuffer(ctx, argv[3]) < 0)))
        goto fail;
    if ((ln / 4) % ((size_t)nh + 1)) {
        JS_ThrowTypeError(ctx, "pathsInto: outNodes length %zu is not a multiple of hops + 1 (%u)", ln / 4, nh + 1);
        goto fail;
    }
    rows = (ln / 4) / ((size_t)nh + 1);
    if (with_t && lt != rows * nh) {
        JS_ThrowTypeError(ctx, "pathsInto: outTypes length %zu must be rows * hops (%llu)", lt, (unsigned long long)(rows * nh));
        goto fail;
    }
    if (with_t && dyn_graph_overlap(pn, ln, pt, lt)) {
        JS_ThrowTypeError(ctx, "pathsInto: outNodes and outTypes share memory");
        goto fail;
    }
    rc = dyn_gk_paths(g, src, ns, masks, nh, (int32_t*)(void*)pn, pt, rows, DYN_GRAPH_PATH_BUDGET, &total);
    free(src);
    free(masks);
    if (rc == DYN_GK_E_BUDGET)
        return JS_ThrowRangeError(ctx, "pathsInto: the walk exceeds its %llu arc-step budget", (unsigned long long)DYN_GRAPH_PATH_BUDGET);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt64(ctx, (int64_t)total);
fail:
    free(src);
    free(masks);
    return JS_EXCEPTION;
}

static int dyn_graph_bool_opt(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsBool(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "Graph.fromEdges: opts.%s must be a boolean", k);
        return -1;
    }
    *out = JS_ToBool(ctx, v);
    return 0;
}

JSValue dyn_graph_from_edges(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "directed", "weighted", "weights", "types", "nodes" };
    const char* op = "Graph.fromEdges";
    JSValue wv = JS_UNDEFINED, tv = JS_UNDEFINED, nv = JS_UNDEFINED;
    JSValueConst o = argc >= 3 ? argv[2] : JS_UNDEFINED;
    int directed = 0, weighted = 0;
    uint32_t nodes = 0, i, top = 0;
    uint8_t *ps, *pd, *pw = NULL, *pt = NULL;
    size_t ls, ld, lw = 0, lt = 0, m;
    dyn_graph_t* g = NULL;
    (void)new_target;
    if (!JS_IsUndefined(o)) {
        if (!JS_IsObject(o))
            return JS_ThrowTypeError(ctx, "%s: options must be an object", op);
        if (dyn_opts_strict(ctx, o, keys, countof(keys)) || dyn_graph_bool_opt(ctx, o, "directed", &directed) || dyn_graph_bool_opt(ctx, o, "weighted", &weighted))
            return JS_EXCEPTION;
        wv = JS_GetPropertyStr(ctx, o, "weights");
        tv = JS_GetPropertyStr(ctx, o, "types");
        nv = JS_GetPropertyStr(ctx, o, "nodes");
        if (JS_IsException(wv) || JS_IsException(tv) || JS_IsException(nv))
            goto fail;
        if (!JS_IsUndefined(nv)) {
            double d;
            if (!JS_IsNumber(nv)) {
                JS_ThrowTypeError(ctx, "%s: opts.nodes must be a number", op);
                goto fail;
            }
            if (JS_ToFloat64(ctx, &d, nv))
                goto fail;
            if (!(d >= 0 && d <= (double)DYN_GRAPH_MAX_NODES) || d != floor(d)) {
                JS_ThrowRangeError(ctx, "%s: opts.nodes must be an integer in [0, 2^26]", op);
                goto fail;
            }
            nodes = (uint32_t)d;
        }
    }
    if (dyn_graph_view(ctx, argv[0], JS_TYPED_ARRAY_INT32, op, "src", &ps, &ls) || dyn_graph_view(ctx, argv[1], JS_TYPED_ARRAY_INT32, op, "dst", &pd, &ld))
        goto fail;
    if (!JS_IsUndefined(wv)) {
        if (!weighted) {
            JS_ThrowTypeError(ctx, "%s: opts.weights needs weighted: true", op);
            goto fail;
        }
        if (dyn_graph_view(ctx, wv, JS_TYPED_ARRAY_FLOAT64, op, "opts.weights", &pw, &lw))
            goto fail;
    }
    if (!JS_IsUndefined(tv) && dyn_graph_view(ctx, tv, JS_TYPED_ARRAY_UINT8, op, "opts.types", &pt, &lt))
        goto fail;
    m = ls / 4;
    if (ld / 4 != m || (pw && lw / 8 != m) || (pt && lt != m)) {
        JS_ThrowTypeError(ctx, "%s: src, dst, weights and types must have the same length", op);
        goto fail;
    }
    for (i = 0; i < m; i++) {
        int32_t u = ((const int32_t*)(const void*)ps)[i], v = ((const int32_t*)(const void*)pd)[i];
        if (u < 0 || v < 0 || (uint32_t)u >= DYN_GRAPH_MAX_NODES || (uint32_t)v >= DYN_GRAPH_MAX_NODES) {
            JS_ThrowRangeError(ctx, "%s: edge %u names node %d, outside [0, 2^26)", op, i, u < 0 || (uint32_t)u >= DYN_GRAPH_MAX_NODES ? u : v);
            goto fail;
        }
        if (pw && !isfinite(((const double*)(const void*)pw)[i])) {
            JS_ThrowRangeError(ctx, "%s: weights[%u] is not finite", op, i);
            goto fail;
        }
        if (pt && pt[i] >= DYN_GRAPH_MAX_TYPES) {
            JS_ThrowRangeError(ctx, "%s: types[%u] = %u is not in [0, 31]", op, i, pt[i]);
            goto fail;
        }
        if ((uint32_t)u + 1 > top)
            top = (uint32_t)u + 1;
        if ((uint32_t)v + 1 > top)
            top = (uint32_t)v + 1;
    }
    if (nodes > top)
        top = nodes;
    g = dyn_gk_new(directed, weighted);
    if (!g || (top && dyn_gk_ensure(g, top - 1)))
        goto oom;
    for (i = 0; i < m; i++)
        if (dyn_gk_add_edge_typed(g, (uint32_t)((const int32_t*)(const void*)ps)[i], (uint32_t)((const int32_t*)(const void*)pd)[i], pw ? ((const double*)(const void*)pw)[i] : 1.0, pt ? pt[i] : 0) < 0)
            goto oom;
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, nv);
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_graph_class_id, g, dyn_gk_free);
oom:
    dyn_gk_free(g);
    JS_ThrowOutOfMemory(ctx);
fail:
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, nv);
    return JS_EXCEPTION;
}

#endif
