#include "docdb-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

JSClassID docdb_stream_class_id;

typedef struct {
    uint64_t seq;
    uint8_t* ev;
} StPending;

typedef struct {
    JSValue resolve, reject;
} StWaiter;

struct DocStream {
    JSValue self;
    JSValue dbobj;
    DocDb* d;
    DynResource* res;
    char* coll;
    size_t coll_n;
    uint64_t pos;
    dd_agg* pipe;
    int lookup;
    int ended;
    int closed;
    int scheduled;
    dd_err err;
    StPending* pend;
    uint32_t npend, cappend, headpend;
    StWaiter* wait;
    uint32_t nwait, capwait;
};

static int st_watching(const DocStream* s, const dd_coll* c)
{
    return !s->coll || (s->coll_n == c->name_n && memcmp(s->coll, c->name, c->name_n) == 0);
}

static int st_push_pending(DocStream* s, uint64_t seq, const uint8_t* ev, size_t n)
{
    StPending* p;
    if (s->npend == s->cappend) {
        uint32_t nc = s->cappend ? s->cappend * 2 : 8;
        StPending* np;
        if (s->headpend) {
            memmove(s->pend, s->pend + s->headpend, (size_t)(s->npend - s->headpend) * sizeof *np);
            s->npend -= s->headpend;
            s->headpend = 0;
        }
        if (s->npend == s->cappend) {
            np = realloc(s->pend, (size_t)nc * sizeof *np);
            if (!np)
                return -1;
            s->pend = np;
            s->cappend = nc;
        }
    }
    p = &s->pend[s->npend];
    p->ev = malloc(n);
    if (!p->ev)
        return -1;
    memcpy(p->ev, ev, n);
    p->seq = seq;
    s->npend++;
    return 0;
}

static void st_free_pending(DocStream* s)
{
    uint32_t i;
    for (i = s->headpend; i < s->npend; i++)
        free(s->pend[i].ev);
    free(s->pend);
    s->pend = NULL;
    s->npend = s->cappend = s->headpend = 0;
}

static int st_sys_event(dyn_bson_w* w, uint64_t seq, const char* op, const dd_coll* c, const dd_db* db, int64_t now)
{
    char tok[17];
    dd_token_hex(seq, tok);
    dyn_bson_w_doc_begin(w);
    dyn_bson_w_sub_begin(w, DYN_BSON_DOC, "_id", 3);
    dyn_bson_w_string(w, "_data", 5, tok, 16);
    dyn_bson_w_sub_end(w);
    dyn_bson_w_string(w, "operationType", 13, op, strlen(op));
    dyn_bson_w_date(w, "wallTime", 8, now);
    if (c) {
        dyn_bson_w_sub_begin(w, DYN_BSON_DOC, "ns", 2);
        if (db->name)
            dyn_bson_w_string(w, "db", 2, db->name, strlen(db->name));
        dyn_bson_w_string(w, "coll", 4, c->name, c->name_n);
        dyn_bson_w_sub_end(w);
    }
    dyn_bson_w_doc_end(w);
    return w->oom ? -1 : 0;
}

void docdb_streams_on_drop(dd_db* db, dd_coll* c, void* owner)
{
    DocDb* d = owner;
    uint64_t drop_seq, inv_seq = 0;
    int64_t now = docdb_now_ms();
    int i, any_coll = 0;
    dyn_bson_w w;
    if (!d)
        return;
    if (d->nstreams == 0) {
        db->unrecorded = db->seq;
        return;
    }
    for (i = 0; i < d->nstreams; i++)
        if (!d->streams[i]->ended && !d->streams[i]->closed && st_watching(d->streams[i], c) && d->streams[i]->coll)
            any_coll = 1;
    drop_seq = ++db->seq;
    if (any_coll)
        inv_seq = ++db->seq;
    dyn_bson_w_init(&w);
    for (i = 0; i < d->nstreams; i++) {
        DocStream* s = d->streams[i];
        uint64_t seq;
        if (s->ended || s->closed || !st_watching(s, c))
            continue;
        if (c->ring_on) {
            const dd_event* e;
            for (seq = s->pos + 1; (e = dd_ring_at(&c->ring, seq)) != NULL; seq = e->seq + 1)
                if (st_push_pending(s, e->seq, e->ev, e->ev_n) < 0)
                    s->err.code = DD_E_OOM;
            if (c->ring.evicted > s->pos && !s->err.code)
                dd_fail(&s->err, DD_E_HISTORYLOST, "the change stream's resume point is no longer in the change ring of %s",
                    c->name);
        }
        dyn_bson_w_reset(&w);
        if (st_sys_event(&w, drop_seq, "drop", c, db, now) < 0 || st_push_pending(s, drop_seq, w.p, w.n) < 0)
            s->err.code = DD_E_OOM;
        if (s->coll) {
            dyn_bson_w_reset(&w);
            if (st_sys_event(&w, inv_seq, "invalidate", NULL, db, now) < 0 || st_push_pending(s, inv_seq, w.p, w.n) < 0)
                s->err.code = DD_E_OOM;
        }
    }
    dyn_bson_w_free(&w);
    db->unrecorded = db->seq;
    docdb_streams_kick(d);
}

static const dd_event* ring_next(const dd_coll* c, uint64_t pos)
{
    return c->ring_on ? dd_ring_at(&c->ring, pos + 1) : NULL;
}

static int st_fetch(DocStream* s, const uint8_t** ev, uint64_t* seq, int* from_pend)
{
    dd_db* db = s->d->db;
    const dd_event* best = NULL;
    int i;
    if (s->err.code)
        return -1;
    for (i = 0; i < db->ncoll; i++) {
        dd_coll* c = db->coll[i];
        const dd_event* e;
        if (!st_watching(s, c) || !c->ring_on)
            continue;
        if (c->ring.evicted > s->pos)
            return dd_fail(&s->err, DD_E_HISTORYLOST, "the change stream's resume point is no longer in the change ring of %s",
                c->name);
        e = ring_next(c, s->pos);
        if (e && (!best || e->seq < best->seq))
            best = e;
    }
    if (s->headpend < s->npend && (!best || s->pend[s->headpend].seq < best->seq)) {
        *ev = s->pend[s->headpend].ev;
        *seq = s->pend[s->headpend].seq;
        *from_pend = 1;
        return 1;
    }
    if (!best)
        return 0;
    *ev = best->ev;
    *seq = best->seq;
    *from_pend = 0;
    return 1;
}

static int st_post(DocStream* s, const uint8_t* ev, dyn_bson_w* w, const uint8_t** out)
{
    dyn_bson_val op, key, id;
    *out = ev;
    if (s->lookup && dyn_bson_find(ev, "operationType", 13, &op) && op.type == DYN_BSON_STRING
        && strcmp((const char*)op.val + 4, "update") == 0 && dyn_bson_find(ev, "documentKey", 11, &key)
        && key.type == DYN_BSON_DOC && dyn_bson_find(key.val, "_id", 3, &id)) {
        dyn_bson_val nsv, cv;
        dd_coll* c = NULL;
        const uint8_t* cur = NULL;
        dyn_bson_iter it;
        if (dyn_bson_find(ev, "ns", 2, &nsv) && nsv.type == DYN_BSON_DOC && dyn_bson_find(nsv.val, "coll", 4, &cv)
            && cv.type == DYN_BSON_STRING)
            c = dd_db_find(s->d->db, (const char*)cv.val + 4, dyn_bson_rd32(cv.val) - 1);
        if (c) {
            uint32_t dn;
            if (dd_coll_find_id(c, id, &dn))
                cur = c->doc[dn];
        }
        dyn_bson_w_reset(w);
        dyn_bson_w_doc_begin(w);
        dyn_bson_iter_init(&it, ev, dyn_bson_rd32(ev));
        while (dyn_bson_iter_next(&it) > 0) {
            dyn_bson_w_value(w, it.key, it.klen, dyn_bson_iter_val(&it));
            if (it.klen == 11 && memcmp(it.key, "documentKey", 11) == 0) {
                if (cur)
                    dyn_bson_w_value(w, "fullDocument", 12, (dyn_bson_val) { DYN_BSON_DOC, cur, dyn_bson_rd32(cur) });
                else
                    dyn_bson_w_null(w, "fullDocument", 12);
            }
        }
        dyn_bson_w_doc_end(w);
        if (w->oom)
            return dd_fail(&s->err, DD_E_OOM, "out of memory");
        *out = w->p;
    }
    return 0;
}

static void st_unlink(DocStream* s)
{
    DocDb* d = s->d;
    int i;
    if (!d)
        return;
    for (i = 0; i < d->nstreams; i++)
        if (d->streams[i] == s) {
            d->streams[i] = d->streams[--d->nstreams];
            break;
        }
    s->d = NULL;
}

void docdb_streams_detach(DocDb* d)
{
    int i;
    for (i = 0; i < d->nstreams; i++)
        d->streams[i]->d = NULL;
    free(d->streams);
    d->streams = NULL;
    d->nstreams = d->capstreams = 0;
}

static void st_advance(DocStream* s, uint64_t seq, int from_pend)
{
    s->pos = seq;
    if (from_pend) {
        free(s->pend[s->headpend].ev);
        s->pend[s->headpend].ev = NULL;
        s->headpend++;
    }
}

static int st_next_value(JSContext* ctx, DocStream* s, JSValue* out)
{
    dyn_bson_w w;
    int rc = 0;
    if (s->closed || s->ended)
        return 0;
    if (!s->d) {
        JS_ThrowTypeError(ctx, "ChangeStream: its Database is closed");
        return -1;
    }
    dyn_bson_w_init(&w);
    for (;;) {
        const uint8_t *ev = NULL, *post = NULL;
        uint64_t seq = 0;
        int fp = 0, inval, r = st_fetch(s, &ev, &seq, &fp);
        dyn_bson_val op;
        JSValue v;
        if (r <= 0) {
            if (r < 0) {
                docdb_throw(ctx, &s->err);
                rc = -1;
            }
            break;
        }
        inval = dyn_bson_find(ev, "operationType", 13, &op) && op.type == DYN_BSON_STRING
            && strcmp((const char*)op.val + 4, "invalidate") == 0;
        if (st_post(s, ev, &w, &post) < 0) {
            docdb_throw(ctx, &s->err);
            rc = -1;
            break;
        }
        if (s->pipe && !inval) {
            dd_rows* rows = NULL;
            dd_err e = { 0 };
            if (dd_agg_run(s->pipe, 1, &post, 1, NULL, 0, docdb_now_ms(), &rows, &e) < 0) {
                s->err = e;
                docdb_throw(ctx, &s->err);
                rc = -1;
                break;
            }
            if (dd_rows_count(rows) == 0) {
                dd_rows_free(rows);
                st_advance(s, seq, fp);
                continue;
            }
            v = docdb_decode(ctx, dd_rows_at(rows, 0));
            dd_rows_free(rows);
        } else {
            v = docdb_decode(ctx, post);
        }
        st_advance(s, seq, fp);
        if (inval)
            s->ended = 1;
        if (JS_IsException(v)) {
            rc = -1;
            break;
        }
        *out = v;
        rc = 1;
        break;
    }
    dyn_bson_w_free(&w);
    if (s->ended)
        st_unlink(s);
    return rc;
}

static JSValue st_result(JSContext* ctx, JSValue value, int done)
{
    JSValue r = JS_NewObject(ctx);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, value);
        return r;
    }
    JS_DefinePropertyValueStr(ctx, r, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, r, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E);
    return r;
}

static JSValue st_settled(JSContext* ctx, int ok, JSValue arg)
{
    JSValue funcs[2], p = JS_NewPromiseCapability(ctx, funcs), r;
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, arg);
        return p;
    }
    r = JS_Call(ctx, funcs[ok ? 0 : 1], JS_UNDEFINED, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

static void st_settle_first(JSContext* ctx, DocStream* s, int ok, JSValue arg)
{
    StWaiter w = s->wait[0];
    JSValue r;
    memmove(s->wait, s->wait + 1, (size_t)(s->nwait - 1) * sizeof *s->wait);
    s->nwait--;
    if (s->res && s->d && s->res->async_users > 0)
        s->res->async_users--;
    r = JS_Call(ctx, ok ? w.resolve : w.reject, JS_UNDEFINED, 1, (JSValueConst*)&arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, w.resolve);
    JS_FreeValue(ctx, w.reject);
}

static void st_pump(JSContext* ctx, DocStream* s)
{
    while (s->nwait > 0) {
        JSValue v = JS_UNDEFINED;
        int r = st_next_value(ctx, s, &v);
        if (r == 0) {
            if (!s->ended && !s->closed)
                break;
            st_settle_first(ctx, s, 1, st_result(ctx, JS_UNDEFINED, 1));
            continue;
        }
        if (r < 0) {
            st_settle_first(ctx, s, 0, JS_GetException(ctx));
            continue;
        }
        st_settle_first(ctx, s, 1, st_result(ctx, v, 0));
    }
}

static JSValue st_pump_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    DocStream* s = argc > 0 ? JS_GetOpaque(argv[0], docdb_stream_class_id) : NULL;
    if (s) {
        s->scheduled = 0;
        st_pump(ctx, s);
    }
    return JS_UNDEFINED;
}

void docdb_streams_kick(DocDb* d)
{
    int i;
    for (i = 0; i < d->nstreams; i++) {
        DocStream* s = d->streams[i];
        if (s->nwait && !s->scheduled && JS_EnqueueJob(d->ctx, st_pump_job, 1, (JSValueConst*)&s->self) == 0)
            s->scheduled = 1;
    }
}

static DocStream* st_get(JSContext* ctx, JSValueConst this_val)
{
    return JS_GetOpaque2(ctx, this_val, docdb_stream_class_id);
}

static JSValue st_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocStream* s = st_get(ctx, this_val);
    JSValue v = JS_UNDEFINED, funcs[2], p;
    StWaiter* nw;
    int r;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->nwait == 0) {
        r = st_next_value(ctx, s, &v);
        if (r < 0)
            return st_settled(ctx, 0, JS_GetException(ctx));
        if (r > 0)
            return st_settled(ctx, 1, st_result(ctx, v, 0));
        if (s->ended || s->closed)
            return st_settled(ctx, 1, st_result(ctx, JS_UNDEFINED, 1));
    }
    if (s->nwait == s->capwait) {
        uint32_t nc = s->capwait ? s->capwait * 2 : 4;
        nw = realloc(s->wait, (size_t)nc * sizeof *nw);
        if (!nw)
            return JS_ThrowOutOfMemory(ctx);
        s->wait = nw;
        s->capwait = nc;
    }
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p))
        return p;
    s->wait[s->nwait].resolve = funcs[0];
    s->wait[s->nwait].reject = funcs[1];
    s->nwait++;
    if (s->res)
        s->res->async_users++;
    return p;
}

static JSValue st_try_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocStream* s = st_get(ctx, this_val);
    JSValue v = JS_NULL;
    int r;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->nwait)
        return JS_ThrowTypeError(ctx, "ChangeStream.tryNext: a next() is already waiting");
    r = st_next_value(ctx, s, &v);
    if (r < 0)
        return JS_EXCEPTION;
    return r ? v : JS_NULL;
}

static void st_close_now(JSContext* ctx, DocStream* s)
{
    if (s->closed)
        return;
    s->closed = 1;
    while (s->nwait)
        st_settle_first(ctx, s, 1, st_result(ctx, JS_UNDEFINED, 1));
    st_unlink(s);
    st_free_pending(s);
}

static JSValue st_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocStream* s = st_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    st_close_now(ctx, s);
    return JS_UNDEFINED;
}

static JSValue st_return(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    DocStream* s = st_get(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    st_close_now(ctx, s);
    return st_settled(ctx, 1, st_result(ctx, JS_UNDEFINED, 1));
}

static JSValue st_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    if (!st_get(ctx, this_val))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val);
}

static JSValue st_token(JSContext* ctx, JSValueConst this_val)
{
    DocStream* s = st_get(ctx, this_val);
    char tok[17];
    JSValue r;
    if (!s)
        return JS_EXCEPTION;
    dd_token_hex(s->pos, tok);
    r = JS_NewObject(ctx);
    if (!JS_IsException(r))
        JS_DefinePropertyValueStr(ctx, r, "_data", JS_NewString(ctx, tok), JS_PROP_C_W_E);
    return r;
}

static JSValue st_closed(JSContext* ctx, JSValueConst this_val)
{
    DocStream* s = st_get(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, s->closed || s->ended);
}

static void st_finalizer(JSRuntime* rt, JSValue val)
{
    DocStream* s = JS_GetOpaque(val, docdb_stream_class_id);
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->nwait; i++) {
        if (s->res && s->d && s->res->async_users > 0)
            s->res->async_users--;
        JS_FreeValueRT(rt, s->wait[i].resolve);
        JS_FreeValueRT(rt, s->wait[i].reject);
    }
    free(s->wait);
    st_unlink(s);
    st_free_pending(s);
    dd_agg_free(s->pipe);
    free(s->coll);
    JS_FreeValueRT(rt, s->dbobj);
    free(s);
}

static void st_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DocStream* s = JS_GetOpaque(val, docdb_stream_class_id);
    uint32_t i;
    if (!s)
        return;
    JS_MarkValue(rt, s->dbobj, mark);
    for (i = 0; i < s->nwait; i++) {
        JS_MarkValue(rt, s->wait[i].resolve, mark);
        JS_MarkValue(rt, s->wait[i].reject, mark);
    }
}

static JSValue st_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "ChangeStream: use collection.watch() or db.watch()");
}

static const JSCFunctionListEntry st_funcs[] = {
    JS_CFUNC_DEF("next", 0, st_next),
    JS_CFUNC_DEF("tryNext", 0, st_try_next),
    JS_CFUNC_DEF("close", 0, st_close),
    JS_CFUNC_DEF("return", 0, st_return),
    JS_CGETSET_DEF("resumeToken", st_token, NULL),
    JS_CGETSET_DEF("closed", st_closed, NULL),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, st_iter),
};

int docdb_register_stream(JSContext* ctx, JSModuleDef* m)
{
    static JSClassDef def = { "ChangeStream", .finalizer = st_finalizer, .gc_mark = st_mark };
    return dyn_register_plain_class(ctx, m, &docdb_stream_class_id, &def, st_funcs, (int)countof(st_funcs), (JSCFunction*)st_ctor,
        "ChangeStream");
}

static int st_token_opt(JSContext* ctx, JSValueConst opts, const char* name, uint64_t* seq, int* present)
{
    JSValue v, dv;
    const char* str;
    size_t n;
    int rc;
    if (docdb_opt(ctx, opts, name, &v) < 0)
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    dv = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "_data") : JS_UNDEFINED;
    JS_FreeValue(ctx, v);
    if (JS_IsException(dv))
        return -1;
    if (!JS_IsString(dv)) {
        JS_FreeValue(ctx, dv);
        docdb_throw_code(ctx, DD_E_BADVALUE, "watch: %s must be a resume token { _data: string }", name);
        return -1;
    }
    str = JS_ToCStringLen(ctx, &n, dv);
    JS_FreeValue(ctx, dv);
    if (!str)
        return -1;
    rc = dd_token_parse(str, n, seq);
    JS_FreeCString(ctx, str);
    if (rc < 0) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "watch: %s is not a resume token of this database", name);
        return -1;
    }
    *present = 1;
    return 0;
}

static int st_pipeline(JSContext* ctx, JSValueConst pipeline, dd_agg** out)
{
    JSValue wrap;
    dyn_bson_w w;
    dyn_bson_val pv;
    dd_err e = { 0 };
    int rc;
    *out = NULL;
    if (JS_IsUndefined(pipeline) || JS_IsNull(pipeline))
        return 0;
    if (!JS_IsArray(ctx, pipeline)) {
        if (!JS_HasException(ctx))
            docdb_throw_code(ctx, 14, "watch: the pipeline must be an array of stage objects");
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
            docdb_throw_code(ctx, 7749, "watch: a pipeline is limited to 1000 stages, got %u", len);
            return -1;
        }
        if (len == 0)
            return 0;
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
            docdb_throw_code(ctx, 14, "watch: the pipeline must be an array of stage objects");
        return -1;
    }
    rc = dd_agg_compile(out, pv.val, ctx, &e);
    dyn_bson_w_free(&w);
    if (rc < 0) {
        docdb_throw(ctx, &e);
        return -1;
    }
    if (!dd_agg_stream_ok(*out)) {
        dd_agg_free(*out);
        *out = NULL;
        docdb_throw_code(ctx, DD_E_BADVALUE,
            "watch: a change stream pipeline may only use $match, $project, $addFields, $set, $unset, $replaceRoot and $replaceWith");
        return -1;
    }
    return 0;
}

JSValue docdb_watch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSValueConst dbobj = this_val;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    DocColl* dc = NULL;
    DocDb* d;
    DocStream* s;
    dd_agg* pipe = NULL;
    uint64_t tok = 0;
    int has_tok = 0, i;
    JSValue fd = JS_UNDEFINED, obj;
    if (!magic) {
        dc = JS_GetOpaque2(ctx, this_val, docdb_coll_class_id);
        if (!dc)
            return JS_EXCEPTION;
        dbobj = dc->dbobj;
    }
    d = docdb_db_get(ctx, dbobj);
    if (!d)
        return JS_EXCEPTION;
    if (st_pipeline(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &pipe) < 0)
        return JS_EXCEPTION;
    if (st_token_opt(ctx, opts, "resumeAfter", &tok, &has_tok) < 0 || (!has_tok && st_token_opt(ctx, opts, "startAfter", &tok, &has_tok) < 0)
        || docdb_opt(ctx, opts, "fullDocument", &fd) < 0) {
        dd_agg_free(pipe);
        return JS_EXCEPTION;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        JS_FreeValue(ctx, fd);
        dd_agg_free(pipe);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->pipe = pipe;
    s->dbobj = JS_DupValue(ctx, dbobj);
    if (JS_IsString(fd)) {
        const char* f = JS_ToCString(ctx, fd);
        if (f && strcmp(f, "updateLookup") == 0)
            s->lookup = 1;
        else if (!f || strcmp(f, "default") != 0) {
            if (f)
                docdb_throw_code(ctx, DD_E_BADVALUE, "watch: fullDocument must be \"default\" or \"updateLookup\"");
            JS_FreeCString(ctx, f);
            goto fail;
        }
        JS_FreeCString(ctx, f);
    } else if (!JS_IsUndefined(fd)) {
        docdb_throw_code(ctx, DD_E_BADVALUE, "watch: fullDocument must be \"default\" or \"updateLookup\"");
        goto fail;
    }
    if (dc && (s->coll = malloc(dc->name_n + 1)) == NULL) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (dc) {
        memcpy(s->coll, dc->name, dc->name_n + 1);
        s->coll_n = dc->name_n;
    }
    s->pos = has_tok ? tok : d->db->seq;
    if (has_tok) {
        if (tok > d->db->seq || tok < d->db->unrecorded) {
            docdb_throw_code(ctx, DD_E_HISTORYLOST, "watch: the resume point is not in this database's change history");
            goto fail;
        }
        for (i = 0; i < d->db->ncoll; i++) {
            dd_coll* c = d->db->coll[i];
            if (c->ring_on && st_watching(s, c) && c->ring.evicted > tok) {
                docdb_throw_code(ctx, DD_E_HISTORYLOST, "watch: the resume point is no longer in the change ring of %s", c->name);
                goto fail;
            }
        }
    }
    if (d->nstreams == d->capstreams) {
        int nc = d->capstreams ? d->capstreams * 2 : 4;
        DocStream** ns = realloc(d->streams, (size_t)nc * sizeof *ns);
        if (!ns) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        d->streams = ns;
        d->capstreams = nc;
    }
    s->res = dyn_res_get(ctx, dbobj, docdb_db_class_id);
    if (!s->res)
        goto fail;
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, docdb_stream_class_id, s, NULL);
    if (JS_IsException(obj))
        goto fail;
    s->self = obj;
    s->d = d;
    d->streams[d->nstreams++] = s;
    d->db->watching = 1;
    JS_FreeValue(ctx, fd);
    return obj;
fail:
    JS_FreeValue(ctx, fd);
    dd_agg_free(s->pipe);
    free(s->coll);
    JS_FreeValue(ctx, s->dbobj);
    free(s);
    return JS_EXCEPTION;
}

#endif
