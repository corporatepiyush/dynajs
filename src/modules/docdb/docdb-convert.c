#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

int docdb_encode_doc(JSContext* ctx, JSValueConst v, dyn_bson_w* w, const char* what)
{
    DynBsonEnc enc;
    const DynBsonJsKinds* k;
    int isarr = JS_IsArray(ctx, v);
    if (isarr < 0)
        return -1;
    if (!JS_IsObject(v) || isarr || JS_IsFunction(ctx, v)) {
        JS_ThrowTypeError(ctx, "%s must be a plain object", what);
        return -1;
    }
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return -1;
    memset(&enc, 0, sizeof enc);
    enc.ctx = ctx;
    enc.w = w;
    enc.k = k;
    return dyn_bsonjs_encode_doc(&enc, v);
}

int docdb_encode_opt_doc(JSContext* ctx, JSValueConst v, dyn_bson_w* w, const char* what)
{
    static const uint8_t empty[5] = { 5, 0, 0, 0, 0 };
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        dyn_bson_w_bytes(w, empty, 5);
        return w->oom ? (JS_ThrowOutOfMemory(ctx), -1) : 0;
    }
    return docdb_encode_doc(ctx, v, w, what);
}

int docdb_encode_insert(JSContext* ctx, JSValueConst v, dyn_bson_w* w)
{
    dyn_bson_w tmp;
    dyn_bson_val id;
    dyn_bson_iter it;
    int first_is_id;
    dyn_bson_w_init(&tmp);
    if (docdb_encode_doc(ctx, v, &tmp, "document") < 0) {
        dyn_bson_w_free(&tmp);
        return -1;
    }
    dyn_bson_iter_init(&it, tmp.p, tmp.n);
    first_is_id = dyn_bson_iter_next(&it) > 0 && it.klen == 3 && memcmp(it.key, "_id", 3) == 0;
    if (first_is_id) {
        dyn_bson_w_bytes(w, tmp.p, tmp.n);
        dyn_bson_w_free(&tmp);
        return w->oom ? (JS_ThrowOutOfMemory(ctx), -1) : 0;
    }
    dyn_bson_w_doc_begin(w);
    if (dyn_bson_find(tmp.p, "_id", 3, &id)) {
        dyn_bson_w_value(w, "_id", 3, id);
    } else {
        uint8_t oid[12];
        dyn_bson_oid_generate(oid);
        dyn_bson_w_oid(w, "_id", 3, oid);
    }
    dyn_bson_iter_init(&it, tmp.p, tmp.n);
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.klen == 3 && memcmp(it.key, "_id", 3) == 0)
            continue;
        dyn_bson_w_value(w, it.key, it.klen, dyn_bson_iter_val(&it));
    }
    dyn_bson_w_doc_end(w);
    dyn_bson_w_free(&tmp);
    if (w->oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

JSValue docdb_decode(JSContext* ctx, const uint8_t* doc)
{
    DynBsonDec d;
    const DynBsonJsKinds* k;
    JSValue r;
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return JS_EXCEPTION;
    dyn_bsonjs_dec_init(&d, ctx, k, DYN_BSONJS_PROMOTE_LONGS | DYN_BSONJS_TRUSTED);
    r = dyn_bsonjs_decode_doc(&d, doc, 0);
    dyn_bsonjs_dec_free(&d);
    return r;
}

JSValue docdb_decode_val(JSContext* ctx, dyn_bson_val v)
{
    DynBsonDec d;
    const DynBsonJsKinds* k;
    JSValue r;
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return JS_EXCEPTION;
    dyn_bsonjs_dec_init(&d, ctx, k, DYN_BSONJS_PROMOTE_LONGS | DYN_BSONJS_TRUSTED);
    r = dyn_bsonjs_decode_val(&d, v);
    dyn_bsonjs_dec_free(&d);
    return r;
}

JSValue docdb_decode_many(JSContext* ctx, const uint8_t* const* docs, uint32_t n, const dd_proj* proj)
{
    DynBsonDec d;
    const DynBsonJsKinds* k;
    JSValue* vals;
    JSValue arr;
    dyn_bson_w w;
    uint32_t i;
    if (n > (uint32_t)INT32_MAX)
        return JS_ThrowRangeError(ctx, "result too large");
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return JS_EXCEPTION;
    vals = malloc((size_t)(n ? n : 1) * sizeof *vals);
    if (!vals)
        return JS_ThrowOutOfMemory(ctx);
    dyn_bson_w_init(&w);
    dyn_bsonjs_dec_init(&d, ctx, k, DYN_BSONJS_PROMOTE_LONGS | DYN_BSONJS_TRUSTED | (n > 1 ? DYN_BSONJS_KEYCACHE : 0));
    for (i = 0; i < n; i++) {
        const uint8_t* src = docs[i];
        if (proj && !proj->empty) {
            dyn_bson_w_reset(&w);
            if (dd_proj_apply(proj, src, &w) < 0) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            src = w.p;
        }
        vals[i] = dyn_bsonjs_decode_doc(&d, src, 0);
        if (JS_IsException(vals[i]))
            goto fail;
    }
    dyn_bsonjs_dec_free(&d);
    dyn_bson_w_free(&w);
    arr = JS_NewArrayFrom(ctx, (int)n, vals);
    free(vals);
    return arr;
fail:
    while (i-- > 0)
        JS_FreeValue(ctx, vals[i]);
    free(vals);
    dyn_bsonjs_dec_free(&d);
    dyn_bson_w_free(&w);
    return JS_EXCEPTION;
}

int docdb_opt(JSContext* ctx, JSValueConst opts, const char* name, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    *out = JS_GetPropertyStr(ctx, opts, name);
    return JS_IsException(*out) ? -1 : 0;
}

int docdb_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, int def, int* out)
{
    JSValue v;
    if (docdb_opt(ctx, opts, name, &v) < 0)
        return -1;
    *out = JS_IsUndefined(v) ? def : JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return *out < 0 ? -1 : 0;
}

int docdb_opt_u64(JSContext* ctx, JSValueConst opts, const char* name, uint64_t* out)
{
    JSValue v;
    double d;
    if (docdb_opt(ctx, opts, name, &v) < 0)
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= 0) || d > 9007199254740991.0 || d != (double)(uint64_t)d) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "%s must be a non-negative integer", name);
        return -1;
    }
    *out = (uint64_t)d;
    return 0;
}

int docdb_opt_doc(JSContext* ctx, JSValueConst opts, const char* name, dyn_bson_w* w, int* present)
{
    JSValue v;
    int rc;
    *present = 0;
    if (docdb_opt(ctx, opts, name, &v) < 0)
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    *present = 1;
    rc = docdb_encode_doc(ctx, v, w, name);
    JS_FreeValue(ctx, v);
    return rc;
}

int docdb_query_init(JSContext* ctx, DocQuery* q, JSValueConst filter, JSValueConst opts)
{
    dyn_bson_w tmp;
    dd_err e = { 0 };
    int present;
    memset(q, 0, sizeof *q);
    dyn_bson_w_init(&q->fbuf);
    if (docdb_encode_opt_doc(ctx, filter, &q->fbuf, "filter") < 0) {
        dyn_bson_w_free(&q->fbuf);
        return -1;
    }
    if (dd_filter_compile(&q->f, q->fbuf.p, ctx, &e) < 0) {
        dyn_bson_w_free(&q->fbuf);
        docdb_throw(ctx, &e);
        return -1;
    }
    dyn_bson_w_init(&tmp);
    if (docdb_opt_doc(ctx, opts, "sort", &tmp, &present) < 0)
        goto fail;
    if (present && dd_sort_parse(&q->sort, tmp.p, &e) < 0) {
        docdb_throw(ctx, &e);
        goto fail;
    }
    dyn_bson_w_reset(&tmp);
    if (docdb_opt_doc(ctx, opts, "projection", &tmp, &present) < 0)
        goto fail;
    if (present) {
        if (dd_proj_parse(&q->proj, tmp.p, &e) < 0) {
            docdb_throw(ctx, &e);
            goto fail;
        }
        q->has_proj = 1;
    }
    if (docdb_opt_u64(ctx, opts, "skip", &q->skip) < 0 || docdb_opt_u64(ctx, opts, "limit", &q->limit) < 0)
        goto fail;
    dyn_bson_w_free(&tmp);
    return 0;
fail:
    dyn_bson_w_free(&tmp);
    docdb_query_free(q);
    return -1;
}

void docdb_query_free(DocQuery* q)
{
    if (q->f.root || q->f.src)
        dd_filter_free(&q->f);
    dd_sort_free(&q->sort);
    if (q->has_proj)
        dd_proj_free(&q->proj);
    dyn_bson_w_free(&q->fbuf);
    memset(q, 0, sizeof *q);
}

int docdb_query_run(JSContext* ctx, dd_coll* c, DocQuery* q, dd_idvec* out, dd_plan* plan_out, dd_stats* st)
{
    dd_plan plan;
    dd_err e = { 0 };
    if (dd_plan_make(c, &q->f, q->sort.n ? &q->sort : NULL, &plan, &e) < 0) {
        docdb_throw(ctx, &e);
        return -1;
    }
    return docdb_query_exec(ctx, c, q, &plan, out, plan_out, st);
}

int docdb_query_exec(JSContext* ctx, dd_coll* c, DocQuery* q, dd_plan* pin, dd_idvec* out, dd_plan* plan_out,
    dd_stats* st)
{
    dd_plan plan = *pin;
    dd_idvec cand = { NULL, 0, 0 };
    dd_err e = { 0 };
    dd_stats local;
    int all = 0, rc;
    dd_view v;
    const dd_sort* s = q->sort.n ? &q->sort : NULL;
    memset(&local, 0, sizeof local);
    if (!st)
        st = &local;
    rc = s && q->limit ? dd_plan_topk(c, &plan, &q->f, q->skip + q->limit < q->skip ? UINT64_MAX : q->skip + q->limit, &cand, st) : 1;
    if (rc < 0 || (rc > 0 && dd_plan_candidates(c, &plan, &cand, &all, st) < 0)) {
        dd_plan_free(&plan);
        dd_idvec_free(&cand);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (st->used_index_sort)
        s = NULL;
    v.docs = c->doc;
    v.ndocs = c->nslots;
    rc = dd_exec_find(v, cand.ids, cand.n, all, &q->f, s, q->skip, q->limit, out, st, &e);
    dd_idvec_free(&cand);
    if (plan_out)
        *plan_out = plan;
    else
        dd_plan_free(&plan);
    if (rc < 0) {
        docdb_throw(ctx, &e);
        return -1;
    }
    return 0;
}

#endif
