#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

static int id_doc(const uint8_t* doc, dyn_bson_w* w)
{
    dyn_bson_val id;
    dyn_bson_w_doc_begin(w);
    if (dyn_bson_find(doc, "_id", 3, &id))
        dyn_bson_w_value(w, "_id", 3, id);
    dyn_bson_w_doc_end(w);
    return w->oom ? -1 : 0;
}

static void emit(dd_coll* c, int op, const uint8_t* doc, const uint8_t* full, const dd_upd_desc* desc, uint32_t docno)
{
    dyn_bson_w k;
    if (!c->ring_on && !c->db->watching) {
        dd_emit(c, op, NULL, NULL, NULL, docno, 0);
        return;
    }
    dyn_bson_w_init(&k);
    if (id_doc(doc, &k) == 0)
        dd_emit(c, op, k.p, full, desc, docno, docdb_now_ms());
    dyn_bson_w_free(&k);
    if (c->db->owner)
        docdb_streams_kick(c->db->owner);
}

int docdb_write_insert(JSContext* ctx, dd_coll* c, const uint8_t* doc, size_t n, dd_err* e)
{
    uint32_t docno;
    (void)ctx;
    if (dd_coll_insert(c, doc, n, &docno, e) < 0)
        return -1;
    c->version++;
    emit(c, DD_EV_INSERT, doc, doc, NULL, docno);
    return (int)docno;
}

int docdb_write_replace(JSContext* ctx, dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, int op,
    const dd_upd_desc* desc, dd_err* e)
{
    (void)ctx;
    if (dd_coll_check_doc(doc, n, 1, e) < 0
        || (op == DD_EV_UPDATE ? dd_coll_update(c, docno, doc, n, e) : dd_coll_replace(c, docno, doc, n, e)) < 0)
        return -1;
    c->version++;
    emit(c, op, doc, doc, desc, docno);
    return 0;
}

int docdb_write_delete(JSContext* ctx, dd_coll* c, uint32_t docno)
{
    dyn_bson_w k;
    (void)ctx;
    dyn_bson_w_init(&k);
    id_doc(c->doc[docno], &k);
    dd_coll_remove(c, docno);
    c->version++;
    if (!k.oom)
        dd_emit(c, DD_EV_DELETE, k.p, NULL, NULL, docno, docdb_now_ms());
    dyn_bson_w_free(&k);
    if (c->db->owner)
        docdb_streams_kick(c->db->owner);
    return 0;
}

static int arr_len(JSContext* ctx, JSValueConst a, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, a, "length");
    int rc;
    if (JS_IsException(v))
        return -1;
    rc = JS_ToInt64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return rc;
}

static JSValue result_obj(JSContext* ctx)
{
    JSValue r = JS_NewObject(ctx);
    if (!JS_IsException(r))
        JS_DefinePropertyValueStr(ctx, r, "acknowledged", JS_TRUE, JS_PROP_C_W_E);
    return r;
}

static void set_num(JSContext* ctx, JSValueConst o, const char* k, double v)
{
    JS_DefinePropertyValueStr(ctx, o, k, JS_NewFloat64(ctx, v), JS_PROP_C_W_E);
}

static JSValue id_value(JSContext* ctx, const uint8_t* doc)
{
    dyn_bson_val id;
    if (!dyn_bson_find(doc, "_id", 3, &id))
        return JS_UNDEFINED;
    return docdb_decode_val(ctx, id);
}

static JSValue coll_insert_one(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, 1);
    dyn_bson_w w;
    dd_err e = { 0 };
    JSValue r, id;
    if (!c)
        return JS_EXCEPTION;
    dyn_bson_w_init(&w);
    if (docdb_encode_insert(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &w) < 0) {
        dyn_bson_w_free(&w);
        return JS_EXCEPTION;
    }
    if (docdb_write_insert(ctx, c, w.p, w.n, &e) < 0) {
        dyn_bson_w_free(&w);
        return docdb_throw(ctx, &e);
    }
    id = id_value(ctx, w.p);
    dyn_bson_w_free(&w);
    if (JS_IsException(id))
        return id;
    r = result_obj(ctx);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, id);
        return r;
    }
    JS_DefinePropertyValueStr(ctx, r, "insertedId", id, JS_PROP_C_W_E);
    return r;
}

static JSValue write_error_entry(JSContext* ctx, uint32_t index, const dd_err* e)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    set_num(ctx, o, "index", index);
    set_num(ctx, o, "code", e->code);
    JS_DefinePropertyValueStr(ctx, o, "codeName", JS_NewString(ctx, dd_code_name(e->code)), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "errmsg", JS_NewString(ctx, e->msg), JS_PROP_C_W_E);
    return o;
}

static JSValue throw_bulk(JSContext* ctx, JSValue errors, uint32_t nerr, const dd_err* first, JSValue result)
{
    JSValue ex;
    char msg[400];
    dd_err e = *first;
    snprintf(msg, sizeof msg, "%u write error(s); first: %s", nerr, first->msg);
    memcpy(e.msg, msg, sizeof e.msg);
    e.msg[sizeof e.msg - 1] = 0;
    docdb_throw(ctx, &e);
    ex = JS_GetException(ctx);
    if (JS_IsObject(ex)) {
        JS_DefinePropertyValueStr(ctx, ex, "writeErrors", errors, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, ex, "result", result, JS_PROP_C_W_E);
    } else {
        JS_FreeValue(ctx, errors);
        JS_FreeValue(ctx, result);
    }
    return JS_Throw(ctx, ex);
}

static JSValue coll_insert_many(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, 1);
    JSValueConst docs = argc > 0 ? argv[0] : JS_UNDEFINED;
    int ordered, isarr;
    int64_t len, i;
    uint32_t nerr = 0, nins = 0;
    dd_err first = { 0 };
    JSValue ids, errors, r;
    dyn_bson_w w;
    if (!c)
        return JS_EXCEPTION;
    isarr = JS_IsArray(ctx, docs);
    if (isarr <= 0)
        return isarr < 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "insertMany(docs): docs must be an array");
    if (docdb_opt_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "ordered", 1, &ordered) < 0)
        return JS_EXCEPTION;
    if (arr_len(ctx, docs, &len))
        return JS_EXCEPTION;
    ids = JS_NewObject(ctx);
    errors = JS_NewArray(ctx);
    if (JS_IsException(ids) || JS_IsException(errors)) {
        JS_FreeValue(ctx, ids);
        JS_FreeValue(ctx, errors);
        return JS_EXCEPTION;
    }
    dyn_bson_w_init(&w);
    for (i = 0; i < len; i++) {
        dd_err e = { 0 };
        JSValue d = JS_GetPropertyUint32(ctx, docs, (uint32_t)i);
        int rc;
        if (JS_IsException(d))
            goto fail;
        dyn_bson_w_reset(&w);
        rc = docdb_encode_insert(ctx, d, &w);
        JS_FreeValue(ctx, d);
        if (rc < 0)
            goto fail;
        c = docdb_coll_get(ctx, this_val, NULL, 1);
        if (!c)
            goto fail;
        if (docdb_write_insert(ctx, c, w.p, w.n, &e) < 0) {
            JSValue ent;
            if (e.code == DD_E_OOM) {
                docdb_throw(ctx, &e);
                goto fail;
            }
            if (!nerr)
                first = e;
            ent = write_error_entry(ctx, (uint32_t)i, &e);
            if (JS_IsException(ent))
                goto fail;
            JS_SetPropertyUint32(ctx, errors, nerr++, ent);
            if (ordered)
                break;
            continue;
        }
        nins++;
        {
            JSValue id = id_value(ctx, w.p);
            if (JS_IsException(id))
                goto fail;
            JS_SetPropertyInt64(ctx, ids, i, id);
        }
    }
    dyn_bson_w_free(&w);
    r = result_obj(ctx);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, ids);
        JS_FreeValue(ctx, errors);
        return r;
    }
    set_num(ctx, r, "insertedCount", nins);
    JS_DefinePropertyValueStr(ctx, r, "insertedIds", ids, JS_PROP_C_W_E);
    if (nerr)
        return throw_bulk(ctx, errors, nerr, &first, r);
    JS_FreeValue(ctx, errors);
    return r;
fail:
    dyn_bson_w_free(&w);
    JS_FreeValue(ctx, ids);
    JS_FreeValue(ctx, errors);
    return JS_EXCEPTION;
}

static JSValue run_find(JSContext* ctx, JSValueConst this_val, JSValueConst filter, JSValueConst opts, int one)
{
    DocDb* d;
    dd_coll* c = docdb_coll_get(ctx, this_val, &d, -1);
    DocQuery q;
    dd_idvec ids = { NULL, 0, 0 };
    const uint8_t** docs;
    uint32_t i;
    uint64_t ep;
    JSValue arr;
    if (!c) {
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return one ? JS_NULL : JS_NewArray(ctx);
    }
    if (docdb_query_init(ctx, &q, filter, opts) < 0)
        return JS_EXCEPTION;
    if (one)
        q.limit = 1;
    if (docdb_query_run(ctx, c, &q, &ids, NULL, NULL) < 0) {
        docdb_query_free(&q);
        return JS_EXCEPTION;
    }
    docs = malloc((size_t)(ids.n ? ids.n : 1) * sizeof *docs);
    if (!docs) {
        docdb_query_free(&q);
        dd_idvec_free(&ids);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < ids.n; i++)
        docs[i] = c->doc[ids.ids[i]];
    ep = dd_db_pin(d->db);
    arr = docdb_decode_many(ctx, docs, ids.n, q.has_proj ? &q.proj : NULL);
    dd_db_unpin(d->db, ep);
    free(docs);
    dd_idvec_free(&ids);
    docdb_query_free(&q);
    if (one && !JS_IsException(arr)) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, 0);
        JS_FreeValue(ctx, arr);
        return JS_IsUndefined(v) ? JS_NULL : v;
    }
    return arr;
}

static JSValue coll_find(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return run_find(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, 0);
}

static JSValue coll_find_one(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return run_find(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, 1);
}

static JSValue coll_count(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, -1);
    DocQuery q;
    dd_idvec ids = { NULL, 0, 0 };
    uint32_t n;
    if (!c)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewInt32(ctx, 0);
    if (docdb_query_init(ctx, &q, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    if (q.f.empty && !q.skip && !q.limit) {
        docdb_query_free(&q);
        return JS_NewUint32(ctx, c->count);
    }
    dd_sort_free(&q.sort);
    {
        dd_plan plan;
        dd_err e = { 0 };
        uint64_t k = 0;
        int rc = 0;
        if (dd_plan_make(c, &q.f, NULL, &plan, &e) < 0) {
            docdb_query_free(&q);
            return docdb_throw(ctx, &e);
        }
        if (!q.f.empty)
            rc = dd_plan_count(c, &plan, &q.f, &k);
        if (rc < 0) {
            dd_plan_free(&plan);
            docdb_query_free(&q);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (rc > 0) {
            dd_plan_free(&plan);
            k = k > q.skip ? k - q.skip : 0;
            if (q.limit && k > q.limit)
                k = q.limit;
            docdb_query_free(&q);
            return JS_NewUint32(ctx, (uint32_t)k);
        }
        rc = docdb_query_exec(ctx, c, &q, &plan, &ids, NULL, NULL);
        if (rc < 0) {
            docdb_query_free(&q);
            dd_idvec_free(&ids);
            return JS_EXCEPTION;
        }
    }
    n = ids.n;
    dd_idvec_free(&ids);
    docdb_query_free(&q);
    return JS_NewUint32(ctx, n);
}

static JSValue coll_estimated(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, -1);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewInt32(ctx, 0);
    return JS_NewUint32(ctx, c->count);
}

typedef struct {
    uint64_t matched, modified, upserted;
    JSValue upserted_id;
    JSValue doc;
} upd_res;

enum { RET_NONE = 0,
    RET_BEFORE,
    RET_AFTER };

static int ensure_id(dyn_bson_w* in, dyn_bson_w* out)
{
    dyn_bson_val id;
    dyn_bson_iter it;
    dyn_bson_w_doc_begin(out);
    if (dyn_bson_find(in->p, "_id", 3, &id)) {
        dyn_bson_w_value(out, "_id", 3, id);
    } else {
        uint8_t oid[12];
        dyn_bson_oid_generate(oid);
        dyn_bson_w_oid(out, "_id", 3, oid);
    }
    dyn_bson_iter_init(&it, in->p, in->n);
    while (dyn_bson_iter_next(&it) > 0)
        if (!(it.klen == 3 && memcmp(it.key, "_id", 3) == 0))
            dyn_bson_w_value(out, it.key, it.klen, dyn_bson_iter_val(&it));
    dyn_bson_w_doc_end(out);
    return out->oom ? -1 : 0;
}

static JSValue decode_proj(JSContext* ctx, const uint8_t* doc, DocQuery* q)
{
    const uint8_t* one[1];
    JSValue arr, v;
    one[0] = doc;
    arr = docdb_decode_many(ctx, one, 1, q->has_proj ? &q->proj : NULL);
    if (JS_IsException(arr))
        return arr;
    v = JS_GetPropertyUint32(ctx, arr, 0);
    JS_FreeValue(ctx, arr);
    return v;
}

static int do_update(JSContext* ctx, JSValueConst this_val, JSValueConst filter, JSValueConst update, JSValueConst opts,
    int multi, int replace_only, int ret, upd_res* res)
{
    DocDb* d;
    dd_coll* tmp;
    dd_coll* c = docdb_coll_read(ctx, this_val, &d, &tmp);
    DocQuery q;
    dd_upd* u = NULL;
    dyn_bson_w ub, out;
    dd_idvec ids = { NULL, 0, 0 };
    dd_err e = { 0 };
    int upsert, rc = -1;
    uint32_t i;
    int64_t now = docdb_now_ms();
    res->matched = res->modified = res->upserted = 0;
    res->upserted_id = JS_UNDEFINED;
    res->doc = JS_NULL;
    if (!c)
        return -1;
    if (docdb_opt_bool(ctx, opts, "upsert", 0, &upsert) < 0 || docdb_query_init(ctx, &q, filter, opts) < 0) {
        dd_coll_free(tmp);
        return -1;
    }
    dyn_bson_w_init(&ub);
    dyn_bson_w_init(&out);
    if (docdb_encode_doc(ctx, update, &ub, replace_only ? "replacement" : "update") < 0)
        goto done;
    if (dd_update_parse(&u, ub.p, NULL, &e) < 0) {
        docdb_throw(ctx, &e);
        goto done;
    }
    if (replace_only && !dd_update_is_replacement(u)) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "Replacement document must not contain atomic operators");
        goto done;
    }
    if (!replace_only && dd_update_is_replacement(u) && ret == RET_NONE) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "Update document requires atomic operators");
        goto done;
    }
    if (!multi)
        q.limit = 1;
    if (docdb_query_run(ctx, c, &q, &ids, NULL, NULL) < 0)
        goto done;
    for (i = 0; i < ids.n; i++) {
        uint32_t no = ids.ids[i];
        const uint8_t* old = c->doc[no];
        dd_match_ctx mc = { .pos = -1, .err = &e };
        dd_upd_desc desc;
        if (!old)
            continue;
        dd_filter_match(&q.f, old, &mc);
        res->matched++;
        dyn_bson_w_reset(&out);
        dd_desc_init(&desc);
        if (dd_update_apply(u, old, mc.pos, 0, now, &out, &desc, &e) < 0) {
            dd_desc_free(&desc);
            docdb_throw(ctx, &e);
            goto done;
        }
        if (ret == RET_BEFORE) {
            res->doc = decode_proj(ctx, old, &q);
            if (JS_IsException(res->doc)) {
                dd_desc_free(&desc);
                goto done;
            }
        }
        if (out.n != dyn_bson_rd32(old) || memcmp(out.p, old, out.n) != 0) {
            if (docdb_write_replace(ctx, c, no, out.p, out.n, dd_update_is_replacement(u) ? DD_EV_REPLACE : DD_EV_UPDATE,
                    &desc, &e)
                < 0) {
                dd_desc_free(&desc);
                docdb_throw(ctx, &e);
                goto done;
            }
            res->modified++;
        }
        dd_desc_free(&desc);
        if (ret == RET_AFTER) {
            res->doc = decode_proj(ctx, c->doc[no], &q);
            if (JS_IsException(res->doc))
                goto done;
        }
    }
    if (ids.n == 0 && upsert) {
        dyn_bson_w seed, fin;
        int r;
        dyn_bson_w_init(&seed);
        dyn_bson_w_init(&fin);
        r = dd_upsert_seed(q.fbuf.p, &seed, &e);
        if (r == 0) {
            dyn_bson_w_reset(&out);
            r = dd_update_apply(u, seed.p, -1, 1, now, &out, NULL, &e);
        }
        if (r == 0 && ensure_id(&out, &fin) < 0)
            r = dd_fail(&e, DD_E_OOM, "out of memory");
        if (r == 0 && dd_coll_check_doc(fin.p, fin.n, 1, &e) < 0)
            r = -1;
        if (r == 0 && tmp && !(c = docdb_coll_get(ctx, this_val, &d, 1))) {
            dyn_bson_w_free(&seed);
            dyn_bson_w_free(&fin);
            goto done;
        }
        if (r == 0 && docdb_write_insert(ctx, c, fin.p, fin.n, &e) < 0)
            r = -1;
        if (r < 0) {
            dyn_bson_w_free(&seed);
            dyn_bson_w_free(&fin);
            docdb_throw(ctx, &e);
            goto done;
        }
        res->upserted = 1;
        res->upserted_id = id_value(ctx, fin.p);
        if (ret == RET_AFTER)
            res->doc = decode_proj(ctx, fin.p, &q);
        dyn_bson_w_free(&seed);
        dyn_bson_w_free(&fin);
        if (JS_IsException(res->upserted_id) || JS_IsException(res->doc))
            goto done;
    }
    rc = 0;
done:
    if (rc < 0) {
        JS_FreeValue(ctx, res->upserted_id);
        JS_FreeValue(ctx, res->doc);
        res->upserted_id = JS_UNDEFINED;
        res->doc = JS_NULL;
    }
    dd_update_free(u);
    dyn_bson_w_free(&ub);
    dyn_bson_w_free(&out);
    dd_idvec_free(&ids);
    docdb_query_free(&q);
    dd_coll_free(tmp);
    return rc;
}

static JSValue update_result(JSContext* ctx, upd_res* res)
{
    JSValue r = result_obj(ctx);
    JS_FreeValue(ctx, res->doc);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, res->upserted_id);
        return r;
    }
    set_num(ctx, r, "matchedCount", (double)res->matched);
    set_num(ctx, r, "modifiedCount", (double)res->modified);
    set_num(ctx, r, "upsertedCount", (double)res->upserted);
    JS_DefinePropertyValueStr(ctx, r, "upsertedId", res->upserted ? res->upserted_id : JS_NULL, JS_PROP_C_W_E);
    return r;
}

static JSValue coll_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    upd_res res;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(filter, update[, options]) needs a filter and an update",
            magic == 2 ? "replaceOne" : magic ? "updateMany"
                                              : "updateOne");
    if (do_update(ctx, this_val, argv[0], argv[1], argc > 2 ? argv[2] : JS_UNDEFINED, magic == 1, magic == 2, RET_NONE, &res) < 0)
        return JS_EXCEPTION;
    return update_result(ctx, &res);
}

static int ret_mode(JSContext* ctx, JSValueConst opts, int* mode)
{
    JSValue v;
    const char* s;
    *mode = RET_BEFORE;
    if (docdb_opt(ctx, opts, "returnDocument", &v) < 0)
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (strcmp(s, "after") == 0)
        *mode = RET_AFTER;
    else if (strcmp(s, "before") != 0) {
        JS_FreeCString(ctx, s);
        docdb_throw_code(ctx, DD_E_BADVALUE, "returnDocument must be 'before' or 'after'");
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue coll_find_one_and(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    upd_res res;
    int mode;
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(filter, %s[, options]) needs two arguments",
            magic ? "findOneAndReplace" : "findOneAndUpdate", magic ? "replacement" : "update");
    if (ret_mode(ctx, opts, &mode) < 0)
        return JS_EXCEPTION;
    if (do_update(ctx, this_val, argv[0], argv[1], opts, 0, magic, mode, &res) < 0)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, res.upserted_id);
    return res.doc;
}

static int do_delete(JSContext* ctx, JSValueConst this_val, JSValueConst filter, JSValueConst opts, int multi,
    uint64_t* deleted, JSValue* doc)
{
    DocDb* d;
    dd_coll* c = docdb_coll_get(ctx, this_val, &d, -1);
    DocQuery q;
    dd_idvec ids = { NULL, 0, 0 };
    uint32_t i;
    *deleted = 0;
    if (doc)
        *doc = JS_NULL;
    if (!c)
        return JS_HasException(ctx) ? -1 : 0;
    if (docdb_query_init(ctx, &q, filter, opts) < 0)
        return -1;
    if (!multi)
        q.limit = 1;
    if (docdb_query_run(ctx, c, &q, &ids, NULL, NULL) < 0) {
        docdb_query_free(&q);
        return -1;
    }
    for (i = 0; i < ids.n; i++) {
        if (!c->doc[ids.ids[i]])
            continue;
        if (doc && i == 0) {
            *doc = decode_proj(ctx, c->doc[ids.ids[i]], &q);
            if (JS_IsException(*doc))
                break;
        }
        docdb_write_delete(ctx, c, ids.ids[i]);
        (*deleted)++;
    }
    dd_idvec_free(&ids);
    docdb_query_free(&q);
    return doc && JS_IsException(*doc) ? -1 : 0;
}

static JSValue coll_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint64_t n;
    JSValue r;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(filter) needs a filter ({} deletes every document)", magic ? "deleteMany" : "deleteOne");
    if (do_delete(ctx, this_val, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, magic, &n, NULL) < 0)
        return JS_EXCEPTION;
    r = result_obj(ctx);
    if (!JS_IsException(r))
        set_num(ctx, r, "deletedCount", (double)n);
    return r;
}

static JSValue coll_find_one_and_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint64_t n;
    JSValue doc;
    if (do_delete(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, 0, &n, &doc) < 0)
        return JS_EXCEPTION;
    return doc;
}

static JSValue bulk_get(JSContext* ctx, JSValueConst o, const char* k)
{
    return JS_GetPropertyStr(ctx, o, k);
}

static JSValue coll_bulk_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const kinds[] = { "insertOne", "updateOne", "updateMany", "replaceOne", "deleteOne", "deleteMany" };
    JSValueConst ops = argc > 0 ? argv[0] : JS_UNDEFINED;
    int ordered, isarr;
    int64_t len, i;
    uint64_t ins = 0, matched = 0, modified = 0, deleted = 0, upserted = 0;
    uint32_t nerr = 0;
    dd_err first = { 0 };
    JSValue errors, ups, r;
    isarr = JS_IsArray(ctx, ops);
    if (isarr <= 0)
        return isarr < 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "bulkWrite(operations): operations must be an array");
    if (docdb_opt_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "ordered", 1, &ordered) < 0)
        return JS_EXCEPTION;
    if (arr_len(ctx, ops, &len))
        return JS_EXCEPTION;
    errors = JS_NewArray(ctx);
    ups = JS_NewObject(ctx);
    if (JS_IsException(errors) || JS_IsException(ups))
        goto fail;
    for (i = 0; i < len; i++) {
        JSValue op = JS_GetPropertyUint32(ctx, ops, (uint32_t)i), body = JS_UNDEFINED;
        int kind = -1, rc = 0;
        size_t k;
        if (JS_IsException(op))
            goto fail;
        for (k = 0; k < countof(kinds) && kind < 0 && JS_IsObject(op); k++) {
            body = bulk_get(ctx, op, kinds[k]);
            if (JS_IsException(body)) {
                JS_FreeValue(ctx, op);
                goto fail;
            }
            if (!JS_IsUndefined(body))
                kind = (int)k;
            else
                JS_FreeValue(ctx, body);
        }
        JS_FreeValue(ctx, op);
        if (kind < 0) {
            JS_ThrowTypeError(ctx, "bulkWrite: operation %lld has no recognised kind", (long long)i);
            goto fail;
        }
        if (kind == 0) {
            JSValue doc = bulk_get(ctx, body, "document");
            JSValue res = JS_IsException(doc) ? JS_EXCEPTION : coll_insert_one(ctx, this_val, 1, (JSValueConst*)&doc);
            JS_FreeValue(ctx, doc);
            if (JS_IsException(res))
                rc = -1;
            else
                ins++;
            JS_FreeValue(ctx, res);
        } else if (kind <= 3) {
            JSValue f = bulk_get(ctx, body, "filter");
            JSValue u = bulk_get(ctx, body, kind == 3 ? "replacement" : "update");
            upd_res res;
            rc = -1;
            if (!JS_IsException(f) && !JS_IsException(u)
                && do_update(ctx, this_val, f, u, body, kind == 2, kind == 3, RET_NONE, &res) == 0) {
                rc = 0;
                matched += res.matched;
                modified += res.modified;
                if (res.upserted) {
                    upserted++;
                    JS_SetPropertyInt64(ctx, ups, i, res.upserted_id);
                } else {
                    JS_FreeValue(ctx, res.upserted_id);
                }
                JS_FreeValue(ctx, res.doc);
            }
            JS_FreeValue(ctx, f);
            JS_FreeValue(ctx, u);
        } else {
            JSValue f = bulk_get(ctx, body, "filter");
            uint64_t n = 0;
            rc = JS_IsException(f) ? -1 : do_delete(ctx, this_val, f, JS_UNDEFINED, kind == 5, &n, NULL);
            deleted += n;
            JS_FreeValue(ctx, f);
        }
        JS_FreeValue(ctx, body);
        if (rc < 0) {
            JSValue ex = JS_GetException(ctx), code, msg, ent;
            dd_err e = { DD_E_INTERNAL, { 0 } };
            int32_t ci = 0;
            code = JS_IsObject(ex) ? JS_GetPropertyStr(ctx, ex, "code") : JS_UNDEFINED;
            msg = JS_IsObject(ex) ? JS_GetPropertyStr(ctx, ex, "message") : JS_UNDEFINED;
            if (!JS_IsNumber(code)) {
                JS_FreeValue(ctx, code);
                JS_FreeValue(ctx, msg);
                JS_Throw(ctx, ex);
                goto fail;
            }
            JS_ToInt32(ctx, &ci, code);
            e.code = ci;
            {
                const char* s = JS_ToCString(ctx, msg);
                if (!s) {
                    JS_FreeValue(ctx, code);
                    JS_FreeValue(ctx, msg);
                    JS_FreeValue(ctx, ex);
                    goto fail;
                }
                snprintf(e.msg, sizeof e.msg, "%s", s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, code);
            JS_FreeValue(ctx, msg);
            JS_FreeValue(ctx, ex);
            if (!nerr)
                first = e;
            ent = write_error_entry(ctx, (uint32_t)i, &e);
            if (JS_IsException(ent))
                goto fail;
            JS_SetPropertyUint32(ctx, errors, nerr++, ent);
            if (ordered)
                break;
        }
    }
    r = result_obj(ctx);
    if (JS_IsException(r))
        goto fail;
    set_num(ctx, r, "insertedCount", (double)ins);
    set_num(ctx, r, "matchedCount", (double)matched);
    set_num(ctx, r, "modifiedCount", (double)modified);
    set_num(ctx, r, "deletedCount", (double)deleted);
    set_num(ctx, r, "upsertedCount", (double)upserted);
    JS_DefinePropertyValueStr(ctx, r, "upsertedIds", ups, JS_PROP_C_W_E);
    if (nerr)
        return throw_bulk(ctx, errors, nerr, &first, r);
    JS_FreeValue(ctx, errors);
    return r;
fail:
    JS_FreeValue(ctx, errors);
    JS_FreeValue(ctx, ups);
    return JS_EXCEPTION;
}

static JSValue coll_distinct(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dd_coll* c = docdb_coll_get(ctx, this_val, NULL, -1);
    DocQuery q;
    dd_idvec ids = { NULL, 0, 0 };
    dd_path p;
    dd_err e = { 0 };
    const char* field;
    size_t fl;
    dd_kmap seen;
    dyn_bson_w vals;
    dyn_ks_buf kb;
    uint32_t i, nv = 0;
    int rc;
    JSValue arr;
    if (!c)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewArray(ctx);
    field = argc > 0 ? JS_ToCStringLen(ctx, &fl, argv[0]) : NULL;
    if (!field)
        return argc > 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "distinct(field[, filter]) needs a field");
    if (dd_path_parse(&p, field, fl, &e) < 0) {
        JS_FreeCString(ctx, field);
        return docdb_throw(ctx, &e);
    }
    JS_FreeCString(ctx, field);
    if (docdb_query_init(ctx, &q, argc > 1 ? argv[1] : JS_UNDEFINED, JS_UNDEFINED) < 0) {
        dd_path_free(&p);
        return JS_EXCEPTION;
    }
    rc = 1;
    if (q.f.empty) {
        int k;
        for (k = 0; k < c->nix && rc == 1; k++)
            if (c->ix[k]->nf == 1 && c->ix[k]->f[0].path.tlen == p.tlen
                && memcmp(c->ix[k]->f[0].path.text, p.text, p.tlen) == 0)
                rc = dd_index_distinct_reps(c, c->ix[k], &ids);
    }
    if (rc < 0) {
        dd_idvec_free(&ids);
        dd_path_free(&p);
        docdb_query_free(&q);
        return JS_ThrowOutOfMemory(ctx);
    }
    ids.n = rc > 0 ? 0 : ids.n;
    if (rc > 0 && docdb_query_run(ctx, c, &q, &ids, NULL, NULL) < 0) {
        dd_idvec_free(&ids);
        dd_path_free(&p);
        docdb_query_free(&q);
        return JS_EXCEPTION;
    }
    dd_kmap_init(&seen);
    dyn_bson_w_init(&vals);
    dyn_bson_w_doc_begin(&vals);
    dyn_ks_init(&kb);
    for (i = 0; i < ids.n; i++) {
        dyn_bson_val v;
        dyn_bson_iter it;
        int isarr;
        if (!dd_path_get_exact(c->doc[ids.ids[i]], &p, &v))
            continue;
        isarr = v.type == DYN_BSON_ARRAY;
        if (isarr)
            dyn_bson_iter_init(&it, v.val, v.vlen);
        for (;;) {
            dyn_bson_val x = v;
            if (isarr) {
                if (dyn_bson_iter_next(&it) <= 0)
                    break;
                x = dyn_bson_iter_val(&it);
            }
            kb.n = 0;
            dyn_ks_value(&kb, x, 0);
            if (!kb.oom && dd_kmap_put(&seen, kb.p, kb.n, nv, NULL) == 0) {
                dyn_bson_w_index_key(&vals, x.type, nv++);
                dyn_bson_w_bytes(&vals, x.val, x.vlen);
            }
            if (!isarr)
                break;
        }
    }
    dyn_bson_w_doc_end(&vals);
    dyn_ks_free(&kb);
    dd_kmap_free(&seen);
    dd_idvec_free(&ids);
    dd_path_free(&p);
    docdb_query_free(&q);
    if (vals.oom) {
        dyn_bson_w_free(&vals);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        dyn_bson_val av = { DYN_BSON_ARRAY, vals.p, (uint32_t)vals.n };
        arr = docdb_decode_val(ctx, av);
    }
    dyn_bson_w_free(&vals);
    return arr;
}

static JSValue coll_drop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocDb* d;
    dd_coll* c = docdb_coll_get(ctx, this_val, &d, -1);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_FALSE;
    dd_db_drop(d->db, c->name, c->name_n);
    d->gen++;
    return JS_TRUE;
}

static JSValue coll_name(JSContext* ctx, JSValueConst this_val)
{
    DocColl* c = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewStringLen(ctx, c->name, c->name_n);
}

static JSValue coll_persist(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocColl* dc = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
    DocDb* d;
    dd_coll* c;
    const char* path;
    int do_fsync;
    JSValue r;
    if (!dc)
        return JS_EXCEPTION;
    c = docdb_coll_get(ctx, this_val, &d, 1);
    if (!c)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "persist(path): path must be a string");
    if (docdb_opt_bool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "fsync", 1, &do_fsync) < 0)
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    r = docdb_persist(ctx, dc->dbobj, d, c, path, do_fsync);
    JS_FreeCString(ctx, path);
    return r;
}

const JSCFunctionListEntry docdb_coll_funcs[] = {
    JS_CGETSET_DEF("name", coll_name, NULL),
    JS_CFUNC_DEF("insertOne", 1, coll_insert_one),
    JS_CFUNC_DEF("insertMany", 1, coll_insert_many),
    JS_CFUNC_DEF("find", 0, coll_find),
    JS_CFUNC_DEF("findOne", 0, coll_find_one),
    JS_CFUNC_DEF("countDocuments", 0, coll_count),
    JS_CFUNC_DEF("estimatedDocumentCount", 0, coll_estimated),
    JS_CFUNC_DEF("distinct", 1, coll_distinct),
    JS_CFUNC_MAGIC_DEF("updateOne", 2, coll_update, 0),
    JS_CFUNC_MAGIC_DEF("updateMany", 2, coll_update, 1),
    JS_CFUNC_MAGIC_DEF("replaceOne", 2, coll_update, 2),
    JS_CFUNC_MAGIC_DEF("deleteOne", 1, coll_delete, 0),
    JS_CFUNC_MAGIC_DEF("deleteMany", 1, coll_delete, 1),
    JS_CFUNC_MAGIC_DEF("findOneAndUpdate", 2, coll_find_one_and, 0),
    JS_CFUNC_MAGIC_DEF("findOneAndReplace", 2, coll_find_one_and, 1),
    JS_CFUNC_DEF("findOneAndDelete", 1, coll_find_one_and_delete),
    JS_CFUNC_DEF("bulkWrite", 1, coll_bulk_write),
    JS_CFUNC_DEF("createIndex", 1, docdb_create_index),
    JS_CFUNC_DEF("dropIndex", 1, docdb_drop_index),
    JS_CFUNC_DEF("listIndexes", 0, docdb_list_indexes),
    JS_CFUNC_DEF("explain", 0, docdb_explain),
    JS_CFUNC_DEF("aggregate", 1, docdb_aggregate),
    JS_CFUNC_MAGIC_DEF("watch", 0, docdb_watch, 0),
    JS_CFUNC_DEF("persist", 1, coll_persist),
    JS_CFUNC_DEF("drop", 0, coll_drop),
};

const int docdb_coll_nfuncs = (int)countof(docdb_coll_funcs);

#endif
