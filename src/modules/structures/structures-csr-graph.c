#include "structures-internal.h"
#include "core/ds/ds-csr-graph.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_csr_t* g;
} dsx_csr_t;

static void dsx_csr_free(void* native)
{
    dsx_csr_t* c = (dsx_csr_t*)native;
    if (!c)
        return;
    dyn_csr_free(c->g);
    dyn_ds_free(c);
}

JSClassID dsx_csr_class_id;

DYN_FINALIZER(dsx_csr)

const JSClassDef dsx_csr_class = { "CSRGraph", .finalizer = dsx_csr_finalizer };

JSValue dsx_csr_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    double dn;
    uint32_t n, i, ne;
    int directed = 1;
    dyn_csr_edge* edges = NULL;
    dsx_csr_t* c;
    JSValue len;
    if (JS_ToFloat64(ctx, &dn, argv[0]))
        return JS_EXCEPTION;
    if (!(dn >= 0) || dn > (double)DSX_MAX_NODES || dn != (double)(uint32_t)dn)
        return JS_ThrowRangeError(ctx, "CSRGraph: nodes must be an integer in 0..%u", DSX_MAX_NODES);
    n = (uint32_t)dn;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue d = JS_GetPropertyStr(ctx, argv[2], "directed");
        if (JS_IsException(d))
            return d;
        if (!JS_IsUndefined(d))
            directed = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
    }
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "CSRGraph: edges must be an array of [src, dst, weight?]");
    len = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToUint32(ctx, &ne, len)) {
        JS_FreeValue(ctx, len);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, len);
    if (ne > 50000000)
        return JS_ThrowRangeError(ctx, "CSRGraph: too many edges");
    edges = (dyn_csr_edge*)dyn_ds_malloc(sizeof(dyn_csr_edge) * (ne ? ne : 1));
    if (!edges)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < ne; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i), a, b, w;
        double da, db, dw = 1;
        if (JS_IsException(e))
            goto fail;
        if (!JS_IsObject(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "CSRGraph: edge %u is not an array", i);
            goto fail;
        }
        a = JS_GetPropertyUint32(ctx, e, 0);
        b = JS_GetPropertyUint32(ctx, e, 1);
        w = JS_GetPropertyUint32(ctx, e, 2);
        JS_FreeValue(ctx, e);
        if (JS_IsException(a) || JS_IsException(b) || JS_IsException(w)) {
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, w);
            goto fail;
        }
        if (dsx_to_f64_free(ctx, &da, a) || dsx_to_f64_free(ctx, &db, b)) {
            JS_FreeValue(ctx, w);
            goto fail;
        }
        if (!JS_IsUndefined(w) && dsx_to_f64_free(ctx, &dw, w))
            goto fail;
        if (!(da >= 0 && da < dn && da == (double)(uint32_t)da && db >= 0 && db < dn && db == (double)(uint32_t)db)) {
            JS_ThrowRangeError(ctx, "CSRGraph: edge %u names a node outside 0..%u", i, n ? n - 1 : 0);
            goto fail;
        }
        if (dw != dw) {
            JS_ThrowRangeError(ctx, "CSRGraph: edge %u has a NaN weight", i);
            goto fail;
        }
        edges[i].src = (uint32_t)da;
        edges[i].dst = (uint32_t)db;
        edges[i].w = dw;
    }
    c = (dsx_csr_t*)dyn_ds_malloc(sizeof(*c));
    if (!c)
        goto oom;
    c->g = dyn_csr_build(n, edges, ne, directed);
    dyn_ds_free(edges);
    if (!c->g) {
        dyn_ds_free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_csr_class_id, c, dsx_csr_free);
oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    dyn_ds_free(edges);
    return JS_EXCEPTION;
}

static int dsx_node_arg(JSContext* ctx, JSValueConst v, dsx_csr_t** pc, JSValueConst this_val, uint32_t* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    *pc = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!*pc)
        return -1;
    if (!(d >= 0) || d >= (double)dyn_csr_nodes((*pc)->g) || d != (double)(uint32_t)d) {
        JS_ThrowRangeError(ctx, "CSRGraph: node out of range");
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue dsx_csr_nodes(JSContext* ctx, JSValueConst this_val, int magic)
{
    dsx_csr_t* c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!c)
        return JS_EXCEPTION;
    return magic ? JS_NewInt64(ctx, (int64_t)dyn_csr_edges(c->g)) : JS_NewInt64(ctx, dyn_csr_nodes(c->g));
}

static JSValue dsx_csr_neighbors(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_csr_t* c;
    uint32_t v, cnt;
    const double* w;
    const uint32_t* nb;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    nb = dyn_csr_neighbors(c->g, v, &w, &cnt);
    if (magic == 2)
        return JS_NewInt64(ctx, cnt);
    if (magic == 1)
        return dsx_typed_copy(ctx, w, cnt, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    return dsx_typed_copy(ctx, nb, cnt, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
}

static JSValue dsx_csr_bfs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c;
    uint32_t v, n;
    int32_t* d;
    JSValue out;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    d = (int32_t*)dyn_ds_malloc(sizeof(int32_t) * (n ? n : 1));
    if (!d || dyn_csr_bfs(c->g, v, d) < 0) {
        dyn_ds_free(d);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = dsx_typed_copy(ctx, d, n, sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    dyn_ds_free(d);
    return out;
}

static JSValue dsx_csr_dijkstra(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c;
    uint32_t v, n;
    double* dist;
    int32_t* pred;
    JSValue obj, a, b;
    (void)argc;
    if (dsx_node_arg(ctx, argv[0], &c, this_val, &v))
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    dist = (double*)dyn_ds_malloc(sizeof(double) * (n ? n : 1));
    pred = (int32_t*)dyn_ds_malloc(sizeof(int32_t) * (n ? n : 1));
    if (!dist || !pred) {
        dyn_ds_free(dist);
        dyn_ds_free(pred);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_csr_dijkstra(c->g, v, dist, pred) < 0) {
        dyn_ds_free(dist);
        dyn_ds_free(pred);
        return JS_ThrowRangeError(ctx, "CSRGraph.dijkstra: negative edge weights are not allowed");
    }
    a = dsx_typed_copy(ctx, dist, n, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    b = dsx_typed_copy(ctx, pred, n, sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    dyn_ds_free(dist);
    dyn_ds_free(pred);
    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "dist", a, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "prev", b, JS_PROP_C_W_E);
    return obj;
}

static JSValue dsx_csr_pagerank(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double damping = 0.85, tol = 1e-10, di = 100;
    dsx_csr_t* c;
    double* r;
    uint32_t n;
    JSValue out;
    if (argc > 0 && JS_IsObject(argv[0])) {
        static const char* const names[3] = { "damping", "iterations", "tolerance" };
        double* dst[3] = { &damping, &di, &tol };
        int k;
        for (k = 0; k < 3; k++) {
            JSValue x = JS_GetPropertyStr(ctx, argv[0], names[k]);
            if (JS_IsException(x))
                return x;
            if (!JS_IsUndefined(x) && dsx_to_f64_free(ctx, dst[k], x))
                return JS_EXCEPTION;
            if (JS_IsUndefined(x))
                JS_FreeValue(ctx, x);
        }
    }
    if (!(damping >= 0 && damping <= 1))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: damping must be in [0, 1]");
    if (!(di >= 1 && di <= 100000))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: iterations must be in 1..100000");
    if (!(tol >= 0))
        return JS_ThrowRangeError(ctx, "CSRGraph.pagerank: tolerance must be >= 0");
    c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    if (!c)
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    r = (double*)dyn_ds_malloc(sizeof(double) * (n ? n : 1));
    if (!r || dyn_csr_pagerank(c->g, damping, (uint32_t)di, tol, r) < 0) {
        dyn_ds_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = dsx_typed_copy(ctx, r, n, sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    dyn_ds_free(r);
    return out;
}

static JSValue dsx_csr_components(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_csr_t* c = dyn_plain_get(ctx, this_val, dsx_csr_class_id);
    uint32_t *lab, n, k;
    JSValue obj, labels;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    n = dyn_csr_nodes(c->g);
    lab = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!lab)
        return JS_ThrowOutOfMemory(ctx);
    k = dyn_csr_components(c->g, lab);
    if (k == UINT32_MAX) {
        dyn_ds_free(lab);
        return JS_ThrowOutOfMemory(ctx);
    }
    labels = dsx_typed_copy(ctx, lab, n, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(lab);
    if (JS_IsException(labels))
        return labels;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, labels);
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, "count", JS_NewInt64(ctx, k), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "labels", labels, JS_PROP_C_W_E);
    return obj;
}

const JSCFunctionListEntry dsx_csr_proto[] = {
    JS_CGETSET_MAGIC_DEF("nodes", dsx_csr_nodes, NULL, 0),
    JS_CGETSET_MAGIC_DEF("edgeCount", dsx_csr_nodes, NULL, 1),
    JS_CFUNC_MAGIC_DEF("neighbors", 1, dsx_csr_neighbors, 0),
    JS_CFUNC_MAGIC_DEF("weights", 1, dsx_csr_neighbors, 1),
    JS_CFUNC_MAGIC_DEF("degree", 1, dsx_csr_neighbors, 2),
    JS_CFUNC_DEF("bfs", 1, dsx_csr_bfs),
    JS_CFUNC_DEF("dijkstra", 1, dsx_csr_dijkstra),
    JS_CFUNC_DEF("pagerank", 1, dsx_csr_pagerank),
    JS_CFUNC_DEF("components", 0, dsx_csr_components),
};

int dsx_csr_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_csr_t* c = JS_GetOpaque(obj, dsx_csr_class_id);
    uint32_t n, v, k, cnt;
    if (!c)
        return -1;
    n = dyn_csr_nodes(c->g);
    if (dyn_ser_u32(w, n) < 0 || dyn_ser_u32(w, (uint32_t)dyn_csr_edges(c->g)) < 0)
        goto oom;
    for (v = 0; v < n; v++) {
        const double* wt;
        const uint32_t* nb = dyn_csr_neighbors(c->g, v, &wt, &cnt);
        for (k = 0; k < cnt; k++)
            if (dyn_ser_u32(w, v) < 0 || dyn_ser_u32(w, nb[k]) < 0 || dyn_ser_f64(w, wt[k]) < 0)
                goto oom;
    }
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
    return -1;
}

JSValue dsx_csr_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n = dyn_de_u32(r), m, i;
    dyn_csr_edge* edges;
    dsx_csr_t* c;
    (void)opts;
    if (!dyn_de_ok(r) || n > DSX_MAX_NODES || dyn_de_count(r, &m, 16) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    edges = (dyn_csr_edge*)dyn_ds_malloc(sizeof(dyn_csr_edge) * (m ? m : 1));
    if (!edges)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < m; i++) {
        edges[i].src = dyn_de_u32(r);
        edges[i].dst = dyn_de_u32(r);
        edges[i].w = dyn_de_f64(r);
        if (!dyn_de_ok(r) || edges[i].src >= n || edges[i].dst >= n || edges[i].w != edges[i].w) {
            dyn_ds_free(edges);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }
    c = (dsx_csr_t*)dyn_ds_malloc(sizeof(*c));
    if (!c) {
        dyn_ds_free(edges);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->g = dyn_csr_build(n, edges, m, 1);
    dyn_ds_free(edges);
    if (!c->g) {
        dyn_ds_free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dsx_csr_class_id, c, dsx_csr_free);
}

#endif
