#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

#define DYN_GRAPH_EXT 0xFFFFFFFFu
#define DYN_GRAPH_EXT_TYPED 0xFFFFFFFEu

static const char* const dyn_graph_keys[] = { "directed", "weighted" };

JSClassID dyn_graph_class_id;

static void dyn_graph_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_gk_free(JS_GetOpaque(val, dyn_graph_class_id));
}

static const JSClassDef dyn_graph_class = {
    "Graph",
    .finalizer = dyn_graph_finalizer,
};

static JSValue dyn_graph_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    int directed = 0, weighted = 0;
    JSValue obj;

    if (argc >= 1 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], dyn_graph_keys,
                countof(dyn_graph_keys)))
            return JS_EXCEPTION;
        JSValue d = JS_GetPropertyStr(ctx, argv[0], "directed");
        JSValue w = JS_GetPropertyStr(ctx, argv[0], "weighted");
        if (JS_IsException(d) || JS_IsException(w)) {
            JS_FreeValue(ctx, d);
            JS_FreeValue(ctx, w);
            return JS_EXCEPTION;
        }
        int bd = JS_ToBool(ctx, d);
        int bw = JS_ToBool(ctx, w);
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, w);
        if (bd < 0 || bw < 0)
            return JS_EXCEPTION;
        directed = bd;
        weighted = bw;
    }
    g = dyn_gk_new(directed, weighted);
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    obj = dyn_plain_wrap(ctx, new_target, dyn_graph_class_id, g, dyn_gk_free);
    return obj;
}

static JSValue dyn_graph_add_node(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (g->n >= DYN_GRAPH_MAX_NODES)
        return JS_ThrowRangeError(ctx, "Graph: a graph holds at most 2^26 nodes");
    if (dyn_gk_ensure(g, g->n))
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt64(ctx, (int64_t)(g->n - 1));
}

static JSValue dyn_graph_node_count(JSContext* ctx, JSValueConst this_val)
{
    dyn_graph_t* g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)g->n);
}

JSClassID dyn_graph_i32_class_id;

static const int32_t dyn_graph_i32_zero;

static JSValue dyn_graph_i32_array(JSContext* ctx, const int32_t* a, size_t n)
{
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (n == 0)
        a = &dyn_graph_i32_zero;
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)a, n * sizeof(int32_t));
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_graph_f64_weights(JSContext* ctx, const dyn_graph_t* g, size_t total)
{
    JSValue ab, out;
    JSValueConst ta_args[3];
    double* w = (double*)malloc((total ? total : 1) * sizeof(double));
    size_t at = 0;
    uint32_t u, i;
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++)
            w[at++] = g->adj[u].e[i].w;
    ab = dyn_nat_ab_take(ctx, w, total * sizeof(double));
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_graph_export_csr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    int32_t *off = NULL, *ed = NULL;
    uint64_t total = 0;
    int rc;
    JSValue joff, jed, res;

    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    rc = dyn_gk_export_csr(g, &off, &ed, &total);
    if (rc == DYN_GK_E_TOO_LARGE)
        return JS_ThrowRangeError(ctx, "exportCSR: too many edges (%llu)",
            (unsigned long long)total);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    joff = dyn_graph_i32_array(ctx, off, (size_t)g->n + 1);
    free(off);
    if (JS_IsException(joff)) {
        free(ed);
        return joff;
    }
    jed = dyn_graph_i32_array(ctx, ed, (size_t)total);
    free(ed);
    if (JS_IsException(jed)) {
        JS_FreeValue(ctx, joff);
        return jed;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, jed);
        JS_FreeValue(ctx, joff);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "offsets", joff, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, jed);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "edges", jed, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (g->weighted) {
        JSValue jw = dyn_graph_f64_weights(ctx, g, (size_t)total);
        if (JS_IsException(jw) || JS_DefinePropertyValueStr(ctx, res, "weights", jw, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    if (g->typed) {
        JSValue jt = dyn_graph_types_array(ctx, g, (size_t)total);
        if (JS_IsException(jt) || JS_DefinePropertyValueStr(ctx, res, "types", jt, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    if (JS_DefinePropertyValueStr(ctx, res, "directed", JS_NewBool(ctx, g->directed), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dyn_graph_mst(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_graph_t* g;
    dyn_gk_edge_t* es;
    dyn_gk_csr_t c;
    uint32_t ne, i;
    double total;
    int rc;
    JSValue res, edges;
    (void)argc;
    (void)argv;
    g = graph_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (g->directed)
        return JS_ThrowTypeError(ctx, "mst requires an undirected graph");
    rc = dyn_gk_csr_of_graph(g, g->weighted, &c);
    if (rc == DYN_GK_E_TOO_LARGE)
        return JS_ThrowRangeError(ctx, "mst: graph too large (more than 2^32-1 arcs)");
    if (rc == 0) {
        rc = dyn_gk_mst(&c, &es, &ne, &total);
        dyn_gk_csr_free(&c);
    }
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    res = JS_NewObject(ctx);
    edges = JS_NewArray(ctx);
    if (JS_IsException(res) || JS_IsException(edges))
        goto fail;
    for (i = 0; i < ne; i++) {
        JSValue tv[3], e;
        tv[0] = JS_NewInt64(ctx, es[i].u);
        tv[1] = JS_NewInt64(ctx, es[i].v);
        tv[2] = JS_NewFloat64(ctx, es[i].w);
        e = JS_NewArrayFrom(ctx, 3, tv);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, edges, i, e, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    free(es);
    if (JS_DefinePropertyValueStr(ctx, res, "weight", JS_NewFloat64(ctx, total), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "edges", edges, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, edges);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
fail:
    free(es);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, edges);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry dyn_graph_proto[] = {
    JS_CFUNC_DEF("addNode", 0, dyn_graph_add_node),
    JS_CFUNC_DEF("addEdge", 2, dyn_graph_add_edge),
    JS_CFUNC_DEF("neighbors", 1, dyn_graph_neighbors),
    JS_CFUNC_DEF("neighborsInto", 2, dyn_graph_neighbors_into),
    JS_CFUNC_DEF("exportCSR", 0, dyn_graph_export_csr),
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
    JS_CFUNC_DEF("shortestPath", 2, dyn_graph_shortest_path),
    JS_CFUNC_DEF("shortestPathTree", 1, dyn_graph_shortest_path_tree),
    JS_CFUNC_DEF("criticalPath", 0, dyn_graph_critical_path),
    JS_CFUNC_DEF("neighborhood", 3, dyn_graph_neighborhood),
    JS_CFUNC_DEF("pathEndpoints", 2, dyn_graph_path_endpoints),
    JS_CFUNC_DEF("pathsInto", 4, dyn_graph_paths_into),
    JS_CFUNC_MAGIC_DEF("pageRank", 1, dyn_graph_algo, GX_PAGERANK),
    JS_CFUNC_MAGIC_DEF("topicPageRank", 2, dyn_graph_algo, GX_TOPIC),
    JS_CFUNC_MAGIC_DEF("labelPropagation", 1, dyn_graph_algo, GX_LPA),
    JS_CFUNC_MAGIC_DEF("louvain", 1, dyn_graph_algo, GX_LOUVAIN),
    JS_CFUNC_MAGIC_DEF("modularity", 2, dyn_graph_algo, GX_MODULARITY),
    JS_CFUNC_MAGIC_DEF("coreNumbers", 0, dyn_graph_algo, GX_CORES),
    JS_CFUNC_MAGIC_DEF("triangles", 0, dyn_graph_algo, GX_TRIANGLES),
    JS_CFUNC_MAGIC_DEF("clustering", 0, dyn_graph_algo, GX_CLUSTERING),
    JS_CFUNC_MAGIC_DEF("betweenness", 1, dyn_graph_algo, GX_BETWEENNESS),
    JS_CFUNC_MAGIC_DEF("closeness", 1, dyn_graph_algo, GX_CLOSENESS),
    JS_CFUNC_MAGIC_DEF("stronglyConnectedComponents", 0, dyn_graph_algo, GX_SCC),
    JS_CFUNC_MAGIC_DEF("weaklyConnectedComponents", 0, dyn_graph_algo, GX_WCC),
    JS_CFUNC_MAGIC_DEF("topologicalLevels", 0, dyn_graph_algo, GX_LEVELS),
    JS_CFUNC_MAGIC_DEF("maxFlow", 2, dyn_graph_algo, GX_MAXFLOW),
    JS_CFUNC_MAGIC_DEF("bipartiteMatching", 1, dyn_graph_algo, GX_MATCHING),
    JS_CFUNC_MAGIC_DEF("linkPrediction", 2, dyn_graph_algo, GX_LINK),
    JS_CGETSET_DEF("nodeCount", dyn_graph_node_count, NULL),
    JS_CGETSET_DEF("edgeCount", dyn_graph_edge_count, NULL),
};

static int dyn_graph_key_cmp(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static int dyn_graph_types_paired(const dyn_graph_t* g)
{
    uint64_t lo = 0, hi = 0, *a, *b;
    uint32_t u, i;
    int ok;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++) {
            uint32_t v = (uint32_t)g->adj[u].e[i].to;
            lo += u < v;
            hi += u > v;
        }
    if (lo != hi)
        return 0;
    a = (uint64_t*)malloc((size_t)(lo ? lo : 1) * sizeof(uint64_t));
    b = (uint64_t*)malloc((size_t)(hi ? hi : 1) * sizeof(uint64_t));
    if (!a || !b) {
        free(a);
        free(b);
        return -1;
    }
    lo = hi = 0;
    for (u = 0; u < g->n; u++)
        for (i = 0; i < g->adj[u].n; i++) {
            uint64_t v = (uint64_t)(uint32_t)g->adj[u].e[i].to, t = g->adj[u].e[i].t;
            if (u < v)
                a[lo++] = ((uint64_t)u << 31) | (v << 5) | t;
            else if (u > v)
                b[hi++] = (v << 31) | ((uint64_t)u << 5) | t;
        }
    qsort(a, (size_t)lo, sizeof(uint64_t), dyn_graph_key_cmp);
    qsort(b, (size_t)hi, sizeof(uint64_t), dyn_graph_key_cmp);
    ok = memcmp(a, b, (size_t)lo * sizeof(uint64_t)) == 0;
    free(a);
    free(b);
    return ok;
}

static int dyn_graph_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_graph_t* g = (dyn_graph_t*)JS_GetOpaque(obj, dyn_graph_class_id);
    uint32_t i, j;

    (void)ctx;
    if (!g)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)(g->directed != 0)) < 0 || dyn_ser_u8(w, (uint8_t)(g->weighted != 0)) < 0 || dyn_ser_u32(w, g->typed ? DYN_GRAPH_EXT_TYPED : DYN_GRAPH_EXT) < 0 || dyn_ser_u32(w, g->n) < 0)
        return -1;
    for (i = 0; i < g->n; i++) {
        const dyn_adj_t* a = &g->adj[i];
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
            if (g->typed && dyn_ser_u8(w, a->e[j].t) < 0)
                return -1;
        }
    }
    return 0;
}

static JSValue dyn_graph_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    dyn_graph_t* g;
    uint32_t n, i, j, self_loops = 0;
    uint64_t deg_total = 0;
    int directed, weighted, varint_form = 0, typed_form = 0;

    (void)opts;
    directed = dyn_de_u8(r) != 0;
    weighted = dyn_de_u8(r) != 0;
    {
        uint32_t first = dyn_de_u32(r);
        if (!dyn_de_ok(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        if (first == DYN_GRAPH_EXT || first == DYN_GRAPH_EXT_TYPED) {
            varint_form = 1;
            typed_form = first == DYN_GRAPH_EXT_TYPED;
            if (dyn_de_count(r, &n, 1) < 0 || n > DYN_GRAPH_MAX_NODES)
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        } else {
            n = first;
            if (n > DYN_GRAPH_MAX_NODES || (uint64_t)n * 4 > (uint64_t)dyn_de_left(r))
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
    }

    g = dyn_gk_new(directed, weighted);
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    if (n) {
        g->adj = (dyn_adj_t*)calloc(n, sizeof(dyn_adj_t));
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
            if (dyn_de_uvarint(r, &d) < 0 || d > UINT32_MAX || d * ((weighted ? 9u : 1u) + (unsigned)typed_form) > (uint64_t)dyn_de_left(r)) {
                dyn_gk_free(g);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            deg = (uint32_t)d;
        } else if (dyn_de_count(r, &deg, weighted ? 12 : 4) < 0) {
            dyn_gk_free(g);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        if (deg) {
            g->adj[i].e = (dyn_edge_t*)malloc((size_t)deg * sizeof(dyn_edge_t));
            if (!g->adj[i].e) {
                dyn_gk_free(g);
                return JS_ThrowOutOfMemory(ctx);
            }
            g->adj[i].cap = deg;
        }
        for (j = 0; j < deg; j++) {
            uint32_t to;
            double wt;
            uint8_t tt;
            if (varint_form) {
                int64_t d;
                if (dyn_de_svarint(r, &d) < 0
                    || (d > 0 && prev > INT64_MAX - d)
                    || (d < 0 && prev < INT64_MIN - d)) {
                    dyn_gk_free(g);
                    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
                }
                prev += d;
                if (prev < 0 || prev > UINT32_MAX) {
                    dyn_gk_free(g);
                    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
                }
                to = (uint32_t)prev;
            } else {
                to = dyn_de_u32(r);
            }
            wt = weighted ? dyn_de_f64(r) : 1.0;
            tt = typed_form ? dyn_de_u8(r) : 0;
            if (tt >= DYN_GRAPH_MAX_TYPES) {
                dyn_gk_free(g);
                return JS_ThrowTypeError(ctx, "malformed Graph record: edge type %u (at most 31)", tt);
            }
            if (!isfinite(wt)) {
                dyn_gk_free(g);
                return JS_ThrowTypeError(ctx,
                    "malformed Graph record: non-finite edge weight");
            }
            if (to >= n) {
                dyn_gk_free(g);
                return JS_ThrowTypeError(ctx,
                    "malformed Graph record: edge to node %u, node count %u",
                    to, n);
            }
            g->adj[i].e[j].to = (int)to;
            g->adj[i].e[j].w = wt;
            g->adj[i].e[j].t = tt;
            g->typed += tt != 0;
            g->adj[i].n++;
            if (to == i)
                self_loops++;
        }
        deg_total += deg;
    }
    if (!dyn_de_ok(r)) {
        dyn_gk_free(g);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if (!directed && typed_form) {
        int paired = dyn_graph_types_paired(g);
        if (paired <= 0) {
            dyn_gk_free(g);
            return paired < 0 ? JS_ThrowOutOfMemory(ctx) : JS_ThrowTypeError(ctx, "malformed Graph record: undirected arcs do not pair up with equal types");
        }
    }
    if (directed) {
        g->m = deg_total;
    } else {
        if (((deg_total + self_loops) & 1u) != 0) {
            dyn_gk_free(g);
            return JS_ThrowTypeError(ctx,
                "malformed Graph record: asymmetric undirected adjacency");
        }
        g->m = (deg_total + self_loops) / 2;
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_graph_class_id, g, dyn_gk_free);
}

static const dyn_codec_t dyn_graph_codec = {
    0,
    DYN_TID_GRAPH,
    "Graph",
    dyn_graph_write,
    dyn_graph_read,
};

static int dyn_graph_init_module(JSContext* ctx, JSModuleDef* m)
{
    dyn_codec_t c = dyn_graph_codec;

    if (dyn_register_plain_class(ctx, m, &dyn_graph_class_id, &dyn_graph_class,
            dyn_graph_proto, countof(dyn_graph_proto),
            dyn_graph_ctor, "Graph")
        < 0)
        return -1;
    {
        JSValueConst ta_args[1];
        JSValue ta, zero = JS_NewInt32(ctx, 0);
        ta_args[0] = zero;
        ta = JS_NewTypedArray(ctx, 1, ta_args, JS_TYPED_ARRAY_INT32);
        if (JS_IsException(ta))
            return -1;
        dyn_graph_i32_class_id = JS_GetClassID(ta);
        JS_FreeValue(ctx, ta);
        JS_FreeValue(ctx, zero);
    }
    if (dyn_graph_algo_register(ctx, m) < 0)
        return -1;
    {
        static const JSCFunctionListEntry statics[] = { JS_CFUNC_DEF("fromEdges", 3, dyn_graph_from_edges) };
        JSValue proto = JS_GetClassProto(ctx, dyn_graph_class_id), ctor;
        if (!JS_IsObject(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        JS_SetPropertyFunctionList(ctx, ctor, statics, countof(statics));
        JS_FreeValue(ctx, ctor);
    }
    c.class_id = dyn_graph_class_id;
    if (dyn_codec_register(&c) < 0)
        return -1;
    return dyn_codec_install_methods(ctx);
}

int js_nat_init_graph(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:graph", dyn_graph_init_module);
    if (!m || JS_AddModuleExport(ctx, m, "Graph") < 0 || JS_AddModuleExport(ctx, m, "graph") < 0)
        return -1;
    return 0;
}

#endif
