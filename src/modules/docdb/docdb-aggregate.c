#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

#define AGG_INLINE_ROWS 2048

typedef struct {
    JSValue dbobj;
    JSValue coll;
    JSValue pipeline;
    DocDb* d;
    DynResource* res;
    dd_agg* a;
    uint8_t* pipe;
    const uint8_t** input;
    uint32_t ninput;
    dd_agg_source* srcs;
    const uint8_t*** srcdocs;
    int nsrc;
    uint64_t epoch;
    int offload;
    int64_t now;
    dd_rows* out;
    dd_err err;
    int rc;
    int counted;
    uint32_t count;
} AggJob;

static void agg_free(JSContext* ctx, AggJob* j)
{
    int i;
    if (j->epoch)
        dd_db_unpin(j->d->db, j->epoch);
    for (i = 0; i < j->nsrc; i++)
        free(j->srcdocs[i]);
    free(j->srcdocs);
    free(j->srcs);
    free(j->input);
    dd_rows_free(j->out);
    dd_agg_free(j->a);
    free(j->pipe);
    if (j->res && j->res->async_users > 0)
        j->res->async_users--;
    if (j->d)
        j->d->async_jobs--;
    JS_FreeValue(ctx, j->dbobj);
    JS_FreeValue(ctx, j->coll);
    JS_FreeValue(ctx, j->pipeline);
    free(j);
}

static int agg_pipeline(JSContext* ctx, JSValueConst pipeline, AggJob* j)
{
    JSValue wrap;
    dyn_bson_w w;
    dyn_bson_val pv;
    int rc;
    if (!JS_IsArray(ctx, pipeline)) {
        if (!JS_HasException(ctx))
            docdb_throw_code(ctx, 14, "aggregate: the pipeline must be an array of stage objects");
        return -1;
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, pipeline, "length");
        uint32_t len = 0;
        int bad = JS_IsException(lv) || JS_ToUint32(ctx, &len, lv) < 0;
        JS_FreeValue(ctx, lv);
        if (bad)
            return -1;
        if (len > 1000) {
            docdb_throw_code(ctx, 7749, "aggregate: a pipeline is limited to 1000 stages, got %u", len);
            return -1;
        }
    }
    wrap = JS_NewObject(ctx);
    if (JS_IsException(wrap))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, wrap, "p", JS_DupValue(ctx, pipeline), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, wrap);
        return -1;
    }
    dyn_bson_w_init(&w);
    rc = docdb_encode_doc(ctx, wrap, &w, "pipeline");
    JS_FreeValue(ctx, wrap);
    if (rc < 0 || !dyn_bson_find(w.p, "p", 1, &pv) || pv.type != DYN_BSON_ARRAY) {
        dyn_bson_w_free(&w);
        if (rc >= 0)
            docdb_throw_code(ctx, 14, "aggregate: the pipeline must be an array of stage objects");
        return -1;
    }
    j->pipe = malloc(pv.vlen);
    if (!j->pipe) {
        dyn_bson_w_free(&w);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(j->pipe, pv.val, pv.vlen);
    dyn_bson_w_free(&w);
    return 0;
}

static int agg_count(JSContext* ctx, dd_coll* c, DocQuery* q, AggJob* j)
{
    dd_plan plan;
    dd_err e = { 0 };
    uint64_t k = 0;
    int rc;
    if (q->f.empty)
        return 0;
    if (dd_plan_make(c, &q->f, NULL, &plan, &e) < 0) {
        docdb_throw(ctx, &e);
        return -1;
    }
    rc = dd_plan_count(c, &plan, &q->f, &k);
    dd_plan_free(&plan);
    if (rc < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (rc == 0)
        return 0;
    k = k > q->skip ? k - q->skip : 0;
    if (q->limit && k > q->limit)
        k = q->limit;
    j->counted = 1;
    j->count = (uint32_t)k;
    return 1;
}

static int agg_prefix(JSContext* ctx, dd_coll* c, AggJob* j)
{
    DocQuery q;
    dd_err e = { 0 };
    dd_idvec ids = { NULL, 0, 0 };
    const uint8_t* m = dd_agg_leading_match(j->a);
    const uint8_t* s = dd_agg_leading_sort(j->a);
    static const uint8_t empty[5] = { 5, 0, 0, 0, 0 };
    uint32_t i;
    memset(&q, 0, sizeof q);
    dyn_bson_w_init(&q.fbuf);
    if (dd_filter_compile(&q.f, m ? m : empty, ctx, &e) < 0 || (s && dd_sort_parse(&q.sort, s, &e) < 0)) {
        docdb_query_free(&q);
        docdb_throw(ctx, &e);
        return -1;
    }
    q.skip = dd_agg_prefix_skip(j->a);
    q.limit = dd_agg_prefix_limit(j->a);
    if (m && dd_agg_count_ok(j->a)) {
        int rc = agg_count(ctx, c, &q, j);
        if (rc != 0) {
            docdb_query_free(&q);
            return rc < 0 ? -1 : 0;
        }
    }
    if (docdb_query_run(ctx, c, &q, &ids, NULL, NULL) < 0) {
        docdb_query_free(&q);
        dd_idvec_free(&ids);
        return -1;
    }
    docdb_query_free(&q);
    j->input = malloc((size_t)(ids.n ? ids.n : 1) * sizeof *j->input);
    if (!j->input) {
        dd_idvec_free(&ids);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < ids.n; i++)
        j->input[i] = c->doc[ids.ids[i]];
    j->ninput = ids.n;
    dd_idvec_free(&ids);
    return 0;
}

static int agg_sources(JSContext* ctx, AggJob* j)
{
    int i, n = dd_agg_lookup_count(j->a);
    if (n == 0)
        return 0;
    j->srcs = calloc((size_t)n, sizeof *j->srcs);
    j->srcdocs = calloc((size_t)n, sizeof *j->srcdocs);
    if (!j->srcs || !j->srcdocs) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++) {
        size_t nl;
        const char* nm = dd_agg_lookup_name(j->a, i, &nl);
        dd_coll* fc = dd_db_find(j->d->db, nm, nl);
        dd_agg_source* s = &j->srcs[j->nsrc];
        s->name = nm;
        s->name_n = nl;
        if (fc && fc->nslots) {
            j->srcdocs[j->nsrc] = malloc((size_t)fc->nslots * sizeof **j->srcdocs);
            if (!j->srcdocs[j->nsrc]) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            memcpy(j->srcdocs[j->nsrc], fc->doc, (size_t)fc->nslots * sizeof **j->srcdocs);
            s->view.docs = j->srcdocs[j->nsrc];
            s->view.ndocs = fc->nslots;
        }
        j->nsrc++;
    }
    return 0;
}

static uint64_t agg_work_rows(const AggJob* j)
{
    uint64_t n = j->ninput;
    int i;
    for (i = 0; i < j->nsrc; i++)
        n += j->srcs[i].view.ndocs;
    return n;
}

static int agg_setup(JSContext* ctx, AggJob* j)
{
    DocDb* d = NULL;
    dd_coll* c = docdb_coll_get(ctx, j->coll, &d, -1);
    dd_err e = { 0 };
    if (!c && JS_HasException(ctx))
        return -1;
    if (!d)
        return -1;
    j->d = d;
    j->res = dyn_res_get(ctx, j->dbobj, docdb_db_class_id);
    if (j->res)
        j->res->async_users++;
    d->async_jobs++;
    j->now = docdb_now_ms();
    if (agg_pipeline(ctx, j->pipeline, j) < 0)
        return -1;
    if (dd_agg_compile(&j->a, j->pipe, ctx, &e) < 0) {
        docdb_throw(ctx, &e);
        return -1;
    }
    if (c && agg_prefix(ctx, c, j) < 0)
        return -1;
    if (agg_sources(ctx, j) < 0)
        return -1;
    j->offload = !dd_agg_has_regex(j->a) && agg_work_rows(j) >= AGG_INLINE_ROWS;
    if (j->offload) {
        j->epoch = dd_db_pin(d->db);
        if (!j->epoch) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    return 0;
}

static JSValue agg_entry(JSContext* ctx, void* arg)
{
    AggJob* j = arg;
    JSValue arr;
    uint32_t i, n;
    if (agg_setup(ctx, j) < 0) {
        agg_free(ctx, j);
        return JS_EXCEPTION;
    }
    if (j->offload)
        dyn_fiber_offload();
    j->rc = j->counted ? dd_agg_run_count(j->a, j->count, j->srcs, j->nsrc, j->now, &j->out, &j->err)
                       : dd_agg_run(j->a, 0, j->input, j->ninput, j->srcs, j->nsrc, j->now, &j->out, &j->err);
    if (j->offload)
        dyn_fiber_onjs();
    if (j->rc < 0) {
        docdb_throw(ctx, &j->err);
        agg_free(ctx, j);
        return JS_EXCEPTION;
    }
    n = dd_rows_count(j->out);
    {
        const uint8_t** rows = malloc((size_t)(n ? n : 1) * sizeof *rows);
        if (!rows) {
            agg_free(ctx, j);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++)
            rows[i] = dd_rows_at(j->out, i);
        arr = docdb_decode_many(ctx, rows, n, NULL);
        free(rows);
    }
    agg_free(ctx, j);
    return arr;
}

JSValue docdb_aggregate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocColl* dc = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
    AggJob* j;
    JSValue p;
    if (!dc)
        return JS_EXCEPTION;
    j = calloc(1, sizeof *j);
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->dbobj = JS_DupValue(ctx, dc->dbobj);
    j->coll = JS_DupValue(ctx, this_val);
    j->pipeline = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    p = dyn_fiber_start(ctx, agg_entry, j);
    if (JS_IsException(p))
        agg_free(ctx, j);
    return p;
}

#endif
