#include <time.h>
#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

JSValue docdb_create_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, 1);
    dyn_bson_w keys, opts;
    dd_index* ix = NULL;
    dd_err e = { 0 };
    JSValue r;
    int rc;
    if (!c)
        return JS_EXCEPTION;
    dyn_bson_w_init(&keys);
    dyn_bson_w_init(&opts);
    if (docdb_encode_doc(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &keys, "index keys") < 0
        || docdb_encode_opt_doc(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &opts, "index options") < 0) {
        dyn_bson_w_free(&keys);
        dyn_bson_w_free(&opts);
        return JS_EXCEPTION;
    }
    rc = dd_index_create(c, keys.p, opts.p, &ix, &e);
    dyn_bson_w_free(&keys);
    dyn_bson_w_free(&opts);
    if (rc < 0)
        return docdb_throw(ctx, &e);
    r = JS_NewString(ctx, ix->name);
    return r;
}

JSValue docdb_drop_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, 0);
    dd_err e = { 0 };
    const char* name;
    size_t n;
    int rc;
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dropIndex(name) needs an index name");
    if (JS_IsObject(argv[0])) {
        dyn_bson_w keys;
        int i;
        dyn_bson_w_init(&keys);
        if (docdb_encode_doc(ctx, argv[0], &keys, "index keys") < 0) {
            dyn_bson_w_free(&keys);
            return JS_EXCEPTION;
        }
        for (i = 0; i < c->nix; i++) {
            dyn_bson_val kv;
            if (dyn_bson_find(c->ix[i]->spec, "key", 3, &kv) && kv.vlen == keys.n && memcmp(kv.val, keys.p, keys.n) == 0)
                break;
        }
        dyn_bson_w_free(&keys);
        if (i == c->nix)
            return docdb_throw_code(ctx, DD_E_INDEXNOTFOUND, "can't find index with key pattern");
        rc = dd_index_drop(c, c->ix[i]->name, strlen(c->ix[i]->name), &e);
        return rc < 0 ? docdb_throw(ctx, &e) : JS_UNDEFINED;
    }
    name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    rc = dd_index_drop(c, name, n, &e);
    JS_FreeCString(ctx, name);
    return rc < 0 ? docdb_throw(ctx, &e) : JS_UNDEFINED;
}

JSValue docdb_list_indexes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const uint8_t id_spec[] = { 0x2e, 0x00, 0x00, 0x00, 0x10, 0x76, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x6b, 0x65,
        0x79, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x10, 0x5f, 0x69, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x6e, 0x61,
        0x6d, 0x65, 0x00, 0x05, 0x00, 0x00, 0x00, 0x5f, 0x69, 0x64, 0x5f, 0x00, 0x00 };
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, -1);
    const uint8_t** specs;
    JSValue r;
    int i;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewArray(ctx);
    specs = malloc((size_t)(c->nix + 1) * sizeof *specs);
    if (!specs)
        return JS_ThrowOutOfMemory(ctx);
    specs[0] = id_spec;
    for (i = 0; i < c->nix; i++)
        specs[i + 1] = c->ix[i]->spec;
    r = docdb_decode_many(ctx, specs, (uint32_t)c->nix + 1, NULL);
    free(specs);
    return r;
}

static void def(JSContext* ctx, JSValueConst o, const char* k, JSValue v)
{
    JS_DefinePropertyValueStr(ctx, o, k, v, JS_PROP_C_W_E);
}

static JSValue ixscan_stage(JSContext* ctx, dd_coll* c, const dd_ixplan* s)
{
    JSValue o = JS_NewObject(ctx);
    dd_index* ix;
    dyn_bson_val kv;
    if (JS_IsException(o) || s->ix < 0 || s->ix >= c->nix)
        return o;
    ix = c->ix[s->ix];
    def(ctx, o, "stage", JS_NewString(ctx, "IXSCAN"));
    def(ctx, o, "indexName", JS_NewString(ctx, ix->name));
    if (dyn_bson_find(ix->spec, "key", 3, &kv))
        def(ctx, o, "keyPattern", docdb_decode_val(ctx, kv));
    def(ctx, o, "isMultiKey", JS_NewBool(ctx, ix->multikey));
    def(ctx, o, "isUnique", JS_NewBool(ctx, ix->unique));
    def(ctx, o, "isSparse", JS_NewBool(ctx, ix->sparse));
    def(ctx, o, "intervals", JS_NewInt32(ctx, s->niv));
    def(ctx, o, "boundFields", JS_NewInt32(ctx, s->field_n));
    def(ctx, o, "estimate", JS_NewFloat64(ctx, (double)s->est));
    return o;
}

static JSValue winning(JSContext* ctx, dd_coll* c, const dd_plan* p, int sorted_in_memory)
{
    JSValue top, cur;
    int i;
    cur = JS_NewObject(ctx);
    if (JS_IsException(cur))
        return cur;
    switch (p->stage) {
    case DD_PLAN_IDHACK:
    case DD_PLAN_EOF:
        def(ctx, cur, "stage", JS_NewString(ctx, dd_plan_stage_name(p->stage)));
        break;
    case DD_PLAN_COLLSCAN:
        if (p->sort_ix >= 0 && p->sort_ix < c->nix) {
            dd_ixplan full;
            memset(&full, 0, sizeof full);
            full.ix = p->sort_ix;
            def(ctx, cur, "stage", JS_NewString(ctx, "FETCH"));
            def(ctx, cur, "inputStage", ixscan_stage(ctx, c, &full));
            {
                JSValue in = JS_GetPropertyStr(ctx, cur, "inputStage");
                def(ctx, in, "direction", JS_NewString(ctx, p->sort_rev ? "backward" : "forward"));
                def(ctx, in, "fullScanForSort", JS_TRUE);
                JS_FreeValue(ctx, in);
            }
        } else {
            def(ctx, cur, "stage", JS_NewString(ctx, "COLLSCAN"));
        }
        break;
    case DD_PLAN_IXSCAN:
        def(ctx, cur, "stage", JS_NewString(ctx, "FETCH"));
        def(ctx, cur, "inputStage", ixscan_stage(ctx, c, &p->scans[0]));
        break;
    default: {
        JSValue inner = JS_NewObject(ctx), arr = JS_NewArray(ctx);
        def(ctx, inner, "stage", JS_NewString(ctx, dd_plan_stage_name(p->stage)));
        for (i = 0; i < p->nscans; i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, ixscan_stage(ctx, c, &p->scans[i]));
        def(ctx, inner, "inputStages", arr);
        def(ctx, cur, "stage", JS_NewString(ctx, "FETCH"));
        def(ctx, cur, "inputStage", inner);
        break;
    }
    }
    if (!sorted_in_memory)
        return cur;
    top = JS_NewObject(ctx);
    def(ctx, top, "stage", JS_NewString(ctx, "SORT"));
    def(ctx, top, "inputStage", cur);
    return top;
}

JSValue docdb_explain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocColl* dc = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
    dd_coll* c;
    dd_coll* tmp;
    DocQuery q;
    dd_plan plan;
    dd_stats st;
    dd_idvec ids = { NULL, 0, 0 };
    struct timespec t0, t1;
    JSValue r, qp, es, rej;
    int i;
    if (!dc)
        return JS_EXCEPTION;
    c = docdb_coll_read(ctx, this_val, NULL, &tmp);
    if (!c)
        return JS_EXCEPTION;
    if (docdb_query_init(ctx, &q, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED) < 0) {
        dd_coll_free(tmp);
        return JS_EXCEPTION;
    }
    memset(&st, 0, sizeof st);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (docdb_query_run(ctx, c, &q, &ids, &plan, &st) < 0) {
        docdb_query_free(&q);
        dd_coll_free(tmp);
        return JS_EXCEPTION;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r = JS_NewObject(ctx);
    qp = JS_NewObject(ctx);
    es = JS_NewObject(ctx);
    rej = JS_NewArray(ctx);
    {
        char ns[300];
        snprintf(ns, sizeof ns, "%s.%s", c->db && c->db->name ? c->db->name : "", c->name);
        def(ctx, qp, "namespace", JS_NewString(ctx, ns));
    }
    def(ctx, qp, "parsedQuery", docdb_decode(ctx, q.fbuf.p));
    def(ctx, qp, "winningPlan", winning(ctx, c, &plan, q.sort.n > 0 && !st.used_index_sort));
    for (i = 0; i < plan.nrejected; i++) {
        JSValue o = JS_NewObject(ctx);
        def(ctx, o, "stage", JS_NewString(ctx, "IXSCAN"));
        def(ctx, o, "indexName", JS_NewString(ctx, plan.rejected[i]));
        JS_SetPropertyUint32(ctx, rej, (uint32_t)i, o);
    }
    def(ctx, qp, "rejectedPlans", rej);
    def(ctx, es, "nReturned", JS_NewFloat64(ctx, (double)ids.n));
    def(ctx, es, "totalKeysExamined", JS_NewFloat64(ctx, (double)st.keys_examined));
    def(ctx, es, "totalDocsExamined", JS_NewFloat64(ctx, (double)st.docs_examined));
    def(ctx, es, "executionTimeMicros",
        JS_NewFloat64(ctx, (double)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000)));
    def(ctx, r, "queryPlanner", qp);
    def(ctx, r, "executionStats", es);
    dd_plan_free(&plan);
    dd_idvec_free(&ids);
    docdb_query_free(&q);
    dd_coll_free(tmp);
    return r;
}

#endif
