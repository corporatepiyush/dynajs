#include "libc-wst.h"

static void rsdc_call_pull_if_needed(JSContext* ctx, SRSDC* c);

void wst_rr_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    SReadReq r = *rr;
    r.ops->chunk(ctx, &r, chunk);
    JS_FreeValue(ctx, r.a);
    JS_FreeValue(ctx, r.b);
}

void wst_rr_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    SReadReq r = *rr;
    r.ops->close(ctx, &r, chunk);
    JS_FreeValue(ctx, r.a);
    JS_FreeValue(ctx, r.b);
}

void wst_rr_error(JSContext* ctx, SReadReq* rr, JSValueConst e)
{
    SReadReq r = *rr;
    r.ops->error(ctx, &r, e);
    JS_FreeValue(ctx, r.a);
    JS_FreeValue(ctx, r.b);
}

static void rrp_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValue o = wst_iter_result(ctx, chunk, 0);
    dyn_settle_value(ctx, rr->a, o);
    JS_FreeValue(ctx, o);
}

static void rrp_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValue o = wst_iter_result(ctx, chunk, 1);
    dyn_settle_value(ctx, rr->a, o);
    JS_FreeValue(ctx, o);
}

static void rrp_error(JSContext* ctx, SReadReq* rr, JSValueConst e)
{
    dyn_settle(ctx, rr->a, e, 1);
}

const SRROps wst_rr_promise_ops = { rrp_chunk, rrp_close, rrp_error };

static void rsdc_free(JSRuntime* rt, SRSDC* c);
static void rsdc_mark_in(JSRuntime* rt, SRSDC* c, JS_MarkFunc* mf);

static void rs_finalizer(JSRuntime* rt, JSValue val)
{
    SRS* s = wst_opaque(val, WSTC_RS);
    if (!s)
        return;
    if (s->c) {
        if (s->bytes)
            wst_rbsc_free(rt, s->c);
        else
            rsdc_free(rt, s->c);
        wst_ctl_drop(rt, s->c);
    }
    JS_FreeValueRT(rt, s->reader);
    JS_FreeValueRT(rt, s->stored_error);
    JS_FreeValueRT(rt, s->ctl);
    JS_FreeValueRT(rt, s->native);
    js_free_rt(rt, s);
}

static void rs_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SRS* s = wst_opaque(val, WSTC_RS);
    if (!s)
        return;
    JS_MarkValue(rt, s->reader, mf);
    JS_MarkValue(rt, s->stored_error, mf);
    JS_MarkValue(rt, s->ctl, mf);
    JS_MarkValue(rt, s->native, mf);
    if (s->c) {
        if (s->bytes)
            wst_rbsc_mark(rt, s->c, mf);
        else
            rsdc_mark_in(rt, s->c, mf);
    }
}

static SReader* wst_reader_opaque(JSValueConst v)
{
    SReader* r = wst_opaque(v, WSTC_RSDR);
    return r ? r : wst_opaque(v, WSTC_RSBR);
}

static void reader_free_reqs(JSRuntime* rt, SQ* q)
{
    uint32_t i;
    for (i = 0; i < q->len; i++) {
        SReadReq* r = sq_at(q, i);
        JS_FreeValueRT(rt, r->a);
        JS_FreeValueRT(rt, r->b);
    }
    sq_release(rt, q);
}

static void reader_finalizer(JSRuntime* rt, JSValue val)
{
    SReader* r = wst_reader_opaque(val);
    if (!r)
        return;
    JS_FreeValueRT(rt, r->stream);
    sdef_free(rt, &r->closed);
    reader_free_reqs(rt, &r->reqs);
    js_free_rt(rt, r);
}

static void reader_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SReader* r = wst_reader_opaque(val);
    uint32_t i;
    if (!r)
        return;
    JS_MarkValue(rt, r->stream, mf);
    sdef_mark(rt, &r->closed, mf);
    for (i = 0; i < r->reqs.len; i++) {
        SReadReq* q = sq_at(&r->reqs, i);
        JS_MarkValue(rt, q->a, mf);
        JS_MarkValue(rt, q->b, mf);
    }
}

void wst_free_value_queue(JSRuntime* rt, SQ* q, double* total)
{
    uint32_t i;
    for (i = 0; i < q->len; i++)
        JS_FreeValueRT(rt, ((SQV*)sq_at(q, i))->v);
    sq_release(rt, q);
    *total = 0;
}

static void rsdc_free(JSRuntime* rt, SRSDC* c)
{
    wst_free_value_queue(rt, &c->q, &c->qtotal);
    JS_FreeValueRT(rt, c->size_fn);
    sa_free(rt, &c->pull);
    sa_free(rt, &c->cancel);
}

static void rsdc_finalizer(JSRuntime* rt, JSValue val)
{
    wst_ctl_fin(rt, wst_opaque(val, WSTC_RSDC));
}

static void rsdc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    wst_ctl_mark(rt, wst_opaque(val, WSTC_RSDC), mf);
}

static void rsdc_mark_in(JSRuntime* rt, SRSDC* c, JS_MarkFunc* mf)
{
    uint32_t i;
    for (i = 0; i < c->q.len; i++)
        JS_MarkValue(rt, ((SQV*)sq_at(&c->q, i))->v, mf);
    JS_MarkValue(rt, c->size_fn, mf);
    sa_mark(rt, &c->pull, mf);
    sa_mark(rt, &c->cancel, mf);
}

JSValue wst_rs_new(JSContext* ctx, JSValueConst proto, SRS** out)
{
    SRS* s;
    JSValue obj = wst_new_obj(ctx, WSTC_RS, proto, sizeof(SRS), (void**)&s);
    if (JS_IsException(obj))
        return obj;
    s->state = RS_READABLE;
    s->reader = JS_UNDEFINED;
    s->stored_error = JS_UNDEFINED;
    s->ctl = JS_UNDEFINED;
    s->native = JS_UNDEFINED;
    *out = s;
    return obj;
}

int wst_rs_locked(SRS* s)
{
    return s->rd != NULL;
}

void wst_rs_close(JSContext* ctx, SRS* s)
{
    SReader* r = s->rd;
    SQ reqs;
    SReadReq rr;
    s->state = RS_CLOSED;
    if (!r)
        return;
    sdef_resolve(ctx, &r->closed, JS_UNDEFINED);
    if (!r->byob) {
        sq_steal(&reqs, &r->reqs);
        sq_init(&r->reqs, sizeof(SReadReq));
        while (reqs.len) {
            sq_shift(&reqs, &rr);
            wst_rr_close(ctx, &rr, JS_UNDEFINED);
        }
        sq_release(JS_GetRuntime(ctx), &reqs);
    }
}

static void reader_error_reqs(JSContext* ctx, SReader* r, JSValueConst e)
{
    SQ reqs;
    SReadReq rr;
    sq_steal(&reqs, &r->reqs);
    sq_init(&r->reqs, sizeof(SReadReq));
    while (reqs.len) {
        sq_shift(&reqs, &rr);
        wst_rr_error(ctx, &rr, e);
    }
    sq_release(JS_GetRuntime(ctx), &reqs);
}

void wst_rs_error(JSContext* ctx, SRS* s, JSValueConst e)
{
    SReader* r = s->rd;
    JSValue old = s->stored_error;
    s->state = RS_ERRORED;
    s->stored_error = JS_DupValue(ctx, e);
    JS_FreeValue(ctx, old);
    if (!r)
        return;
    sdef_reject(ctx, &r->closed, e);
    JS_PromiseMarkHandled(ctx, r->closed.p);
    reader_error_reqs(ctx, r, e);
}

void wst_rs_fulfill_read_request(JSContext* ctx, SRS* s, JSValueConst chunk, int done)
{
    SReadReq rr;
    sq_shift(&s->rd->reqs, &rr);
    if (done)
        wst_rr_close(ctx, &rr, JS_UNDEFINED);
    else
        wst_rr_chunk(ctx, &rr, chunk);
}

void wst_rs_fulfill_read_into_request(JSContext* ctx, SRS* s, JSValueConst chunk, int done)
{
    SReadReq rr;
    sq_shift(&s->rd->reqs, &rr);
    if (done)
        wst_rr_close(ctx, &rr, chunk);
    else
        wst_rr_chunk(ctx, &rr, chunk);
}

int wst_rs_num_read_requests(SRS* s)
{
    return s->rd ? (int)s->rd->reqs.len : 0;
}

int wst_rs_add_read_request(JSContext* ctx, SRS* s, SReadReq* rr)
{
    SReadReq* slot = sq_push(ctx, &s->rd->reqs);
    if (!slot) {
        JS_FreeValue(ctx, rr->a);
        JS_FreeValue(ctx, rr->b);
        return -1;
    }
    *slot = *rr;
    return 0;
}

static JSValue rs_ctl_cancel_steps(JSContext* ctx, SRS* s, JSValueConst reason);
static void rs_ctl_release_steps(JSContext* ctx, SRS* s);
static void rs_ctl_pull_steps(JSContext* ctx, SRS* s, SReadReq* rr);

JSValue wst_rs_cancel(JSContext* ctx, SRS* s, JSValueConst reason)
{
    SReader* r;
    JSValue p, q, pin;
    s->disturbed = 1;
    if (s->state == RS_CLOSED)
        return wst_resolved_undef(ctx);
    if (s->state == RS_ERRORED)
        return wst_rejected(ctx, s->stored_error);
    pin = JS_DupValue(ctx, s->obj);
    wst_rs_close(ctx, s);
    r = s->rd;
    if (r && r->byob) {
        SQ reqs;
        SReadReq rr;
        sq_steal(&reqs, &r->reqs);
        sq_init(&r->reqs, sizeof(SReadReq));
        while (reqs.len) {
            sq_shift(&reqs, &rr);
            wst_rr_close(ctx, &rr, JS_UNDEFINED);
        }
        sq_release(JS_GetRuntime(ctx), &reqs);
    }
    p = rs_ctl_cancel_steps(ctx, s, reason);
    JS_FreeValue(ctx, pin);
    if (JS_IsException(p))
        return p;
    q = wst_transform_undef(ctx, p);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue reader_alloc(JSContext* ctx, int byob, JSValueConst proto, SReader** out)
{
    SReader* r;
    JSValue obj = wst_new_obj(ctx, byob ? WSTC_RSBR : WSTC_RSDR, proto, sizeof(SReader), (void**)&r);
    if (JS_IsException(obj))
        return obj;
    r->byob = byob;
    r->stream = JS_UNDEFINED;
    sdef_init(&r->closed);
    sq_init(&r->reqs, sizeof(SReadReq));
    *out = r;
    return obj;
}

static int reader_setup(JSContext* ctx, SReader* r, SRS* s)
{
    if (wst_rs_locked(s)) {
        wst_throw(ctx, 0, "the ReadableStream is locked to a reader");
        return -1;
    }
    if (r->byob && !s->bytes) {
        wst_throw(ctx, 0, "a BYOB reader needs a byte stream (type: \"bytes\")");
        return -1;
    }
    r->stream = JS_DupValue(ctx, s->obj);
    r->s = s;
    s->reader = JS_DupValue(ctx, r->obj);
    s->rd = r;
    if (s->state == RS_READABLE)
        return sdef_new(ctx, &r->closed);
    if (s->state == RS_CLOSED)
        return sdef_new_resolved(ctx, &r->closed, JS_UNDEFINED);
    return sdef_new_rejected(ctx, &r->closed, s->stored_error);
}

JSValue wst_rs_acquire_reader(JSContext* ctx, SRS* s, int byob, SReader** out)
{
    SReader* r;
    JSValue obj;
    if (wst_rs_locked(s))
        return wst_throw(ctx, 0, "the ReadableStream is locked to a reader");
    if (byob && !s->bytes)
        return wst_throw(ctx, 0, "a BYOB reader needs a byte stream (type: \"bytes\")");
    obj = reader_alloc(ctx, byob, JS_UNDEFINED, &r);
    if (JS_IsException(obj))
        return obj;
    if (reader_setup(ctx, r, s) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (out)
        *out = r;
    return obj;
}

void wst_reader_release(JSContext* ctx, SReader* r)
{
    SRS* s = r->s;
    JSValue e = wst_err(ctx, 0, "the reader's lock was released"), so, ro;
    if (s->state == RS_READABLE) {
        sdef_reject(ctx, &r->closed, e);
    } else {
        JSValue e2 = wst_err(ctx, 0, "the reader's lock was released");
        sdef_new_rejected(ctx, &r->closed, e2);
        JS_FreeValue(ctx, e2);
    }
    JS_FreeValue(ctx, e);
    JS_PromiseMarkHandled(ctx, r->closed.p);
    rs_ctl_release_steps(ctx, s);
    ro = s->reader;
    so = r->stream;
    s->reader = JS_UNDEFINED;
    s->rd = NULL;
    r->stream = JS_UNDEFINED;
    r->s = NULL;
    e = wst_err(ctx, 0, "the reader's lock was released");
    reader_error_reqs(ctx, r, e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, so);
    JS_FreeValue(ctx, ro);
}

void wst_reader_read(JSContext* ctx, SReader* r, SReadReq* rr)
{
    SRS* s = r->s;
    s->disturbed = 1;
    if (s->state == RS_CLOSED)
        wst_rr_close(ctx, rr, JS_UNDEFINED);
    else if (s->state == RS_ERRORED)
        wst_rr_error(ctx, rr, s->stored_error);
    else
        rs_ctl_pull_steps(ctx, s, rr);
}

JSValue wst_reader_read_promise(JSContext* ctx, SReader* r)
{
    SReadReq rr;
    JSValue p = JS_NewPromisePending(ctx);
    if (JS_IsException(p))
        return p;
    rr.ops = &wst_rr_promise_ops;
    rr.a = JS_DupValue(ctx, p);
    rr.b = JS_UNDEFINED;
    wst_reader_read(ctx, r, &rr);
    return wst_ret(ctx, p);
}

static int rsdc_can_close_or_enqueue(SRSDC* c)
{
    return !c->close_requested && c->s->state == RS_READABLE;
}

static double rsdc_desired_num(SRSDC* c)
{
    return c->hwm - c->qtotal;
}

static int rsdc_should_call_pull(SRSDC* c)
{
    SRS* s = c->s;
    if (!rsdc_can_close_or_enqueue(c))
        return 0;
    if (!c->started)
        return 0;
    if (wst_rs_locked(s) && wst_rs_num_read_requests(s) > 0)
        return 1;
    return rsdc_desired_num(c) > 0;
}

static void rsdc_clear_algorithms(JSContext* ctx, SRSDC* c)
{
    JSValue f = c->size_fn;
    sa_clear(ctx, &c->pull);
    sa_clear(ctx, &c->cancel);
    c->size_fn = JS_UNDEFINED;
    c->size_set = 0;
    JS_FreeValue(ctx, f);
}

void wst_rsdc_error(JSContext* ctx, SRSDC* c, JSValueConst e)
{
    SRS* s = c->s;
    if (s->state != RS_READABLE)
        return;
    wst_free_value_queue(JS_GetRuntime(ctx), &c->q, &c->qtotal);
    rsdc_clear_algorithms(ctx, c);
    wst_rs_error(ctx, s, e);
}

static void rsdc_pull_done(JSContext* ctx, SRSDC* c, int is_reject, JSValueConst arg)
{
    if (is_reject) {
        wst_rsdc_error(ctx, c, arg);
        return;
    }
    c->pulling = 0;
    if (c->pull_again) {
        c->pull_again = 0;
        rsdc_call_pull_if_needed(ctx, c);
    }
}

JSValue wst_rs_ctl(JSContext* ctx, SRS* s)
{
    return wst_ctl_get(ctx, s->bytes ? WSTC_RBSC : WSTC_RSDC, s->c, s->obj, &s->ctl);
}

static JSValue rs_pull_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SRS* s = wst_opaque(data, WSTC_RS);
    (void)magic;
    if (s->bytes)
        wst_rbsc_pull_done(ctx, s->c, is_reject, arg);
    else
        rsdc_pull_done(ctx, s->c, is_reject, arg);
    return JS_UNDEFINED;
}

static void rsdc_call_pull_if_needed(JSContext* ctx, SRSDC* c)
{
    JSValue pin;
    JSValueConst arg;
    if (!rsdc_should_call_pull(c))
        return;
    if (c->pulling) {
        c->pull_again = 1;
        return;
    }
    c->pulling = 1;
    pin = JS_DupValue(ctx, c->s->obj);
    arg = sa_kind(&c->pull) != SA_JS ? JS_UNDEFINED : !JS_IsUndefined(c->obj) ? c->obj : wst_rs_ctl(ctx, c->s);
    if (JS_IsException(arg)) {
        JSValue e = JS_GetException(ctx);
        wst_rsdc_error(ctx, c, e);
        JS_FreeValue(ctx, e);
    } else {
        sa_call_upon(ctx, &c->pull, 1, &arg, c->s->obj);
    }
    JS_FreeValue(ctx, pin);
}

void wst_rsdc_close(JSContext* ctx, SRSDC* c)
{
    if (!rsdc_can_close_or_enqueue(c))
        return;
    c->close_requested = 1;
    if (c->q.len == 0) {
        rsdc_clear_algorithms(ctx, c);
        wst_rs_close(ctx, c->s);
    }
}

int wst_rsdc_enqueue(JSContext* ctx, SRSDC* c, JSValueConst chunk)
{
    SRS* s = c->s;
    JSValue size, pin;
    int ret = 0;
    if (!rsdc_can_close_or_enqueue(c))
        return 0;
    if (wst_rs_locked(s) && wst_rs_num_read_requests(s) > 0) {
        wst_rs_fulfill_read_request(ctx, s, chunk, 0);
    } else {
        pin = JS_DupValue(ctx, s->obj);
        if (wst_size_of(ctx, c->size_fn, c->size_set, chunk, &size) < 0
            || (ret = wst_queue_push(ctx, &c->q, &c->qtotal, chunk, size), JS_FreeValue(ctx, size), ret < 0)) {
            JSValue e;
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, pin);
                return -1;
            }
            e = JS_GetException(ctx);
            wst_rsdc_error(ctx, c, e);
            JS_FreeValue(ctx, pin);
            JS_Throw(ctx, e);
            return -1;
        }
        JS_FreeValue(ctx, pin);
    }
    rsdc_call_pull_if_needed(ctx, c);
    return 0;
}

JSValue wst_rsdc_desired(JSContext* ctx, SRSDC* c)
{
    if (c->s->state == RS_ERRORED)
        return JS_NULL;
    if (c->s->state == RS_CLOSED)
        return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, rsdc_desired_num(c));
}

int wst_rsdc_has_backpressure(SRSDC* c)
{
    return !rsdc_should_call_pull(c);
}

int wst_rsdc_can_close_or_enqueue(SRSDC* c)
{
    return rsdc_can_close_or_enqueue(c);
}

static void rsdc_pull_steps(JSContext* ctx, SRSDC* c, SReadReq* rr)
{
    SRS* s = c->s;
    JSValue chunk;
    if (c->q.len > 0) {
        chunk = wst_queue_shift(&c->q, &c->qtotal);
        if (c->close_requested && c->q.len == 0) {
            rsdc_clear_algorithms(ctx, c);
            wst_rs_close(ctx, s);
        } else {
            rsdc_call_pull_if_needed(ctx, c);
        }
        wst_rr_chunk(ctx, rr, chunk);
        JS_FreeValue(ctx, chunk);
    } else {
        wst_rs_add_read_request(ctx, s, rr);
        rsdc_call_pull_if_needed(ctx, c);
    }
}

static JSValue rs_ctl_cancel_steps(JSContext* ctx, SRS* s, JSValueConst reason)
{
    SRSDC* c;
    JSValue r;
    if (s->bytes)
        return wst_rbsc_cancel_steps(ctx, s->c, reason);
    c = s->c;
    wst_free_value_queue(JS_GetRuntime(ctx), &c->q, &c->qtotal);
    r = sa_call(ctx, &c->cancel, 1, &reason);
    rsdc_clear_algorithms(ctx, c);
    return r;
}

static void rs_ctl_release_steps(JSContext* ctx, SRS* s)
{
    if (s->bytes)
        wst_rbsc_release_steps(ctx, s->c);
}

static void rs_ctl_pull_steps(JSContext* ctx, SRS* s, SReadReq* rr)
{
    if (s->bytes)
        wst_rbsc_pull_steps(ctx, s->c, rr);
    else
        rsdc_pull_steps(ctx, s->c, rr);
}

static JSValue rsdc_start_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SRS* s = wst_opaque(data, WSTC_RS);
    SRSDC* c = s->c;
    (void)magic;
    if (is_reject) {
        wst_rsdc_error(ctx, c, arg);
        return JS_UNDEFINED;
    }
    c->started = 1;
    rsdc_call_pull_if_needed(ctx, c);
    return JS_UNDEFINED;
}

static JSValue rsdc_start_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)argc;
    return rsdc_start_rx(ctx, argv[0], 0, 0, argv[1]);
}

int wst_rsdc_setup(JSContext* ctx, SRS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* pull, SAlgo* cancel, double hwm, JSValueConst size_fn, int size_set)
{
    SRSDC* c;
    JSValue cobj, r;
    int i;

    c = js_mallocz(ctx, sizeof(SRSDC));
    if (!c)
        return -1;
    c->obj = JS_UNDEFINED;
    c->stream = JS_UNDEFINED;
    c->s = s;
    sq_init(&c->q, sizeof(SQV));
    c->size_fn = JS_DupValue(ctx, size_fn);
    c->size_set = size_set;
    c->hwm = hwm;
    c->pull = *pull;
    c->cancel = *cancel;
    sa_init(pull);
    sa_init(cancel);
    s->c = c;
    s->bytes = 0;
    if (JS_IsUndefined(start_fn)) {
        r = JS_DupValue(ctx, start_value);
    } else {
        cobj = wst_rs_ctl(ctx, s);
        if (JS_IsException(cobj))
            return -1;
        r = JS_Call(ctx, start_fn, start_self, 1, (JSValueConst*)&cobj);
        if (JS_IsException(r))
            return -1;
    }
    i = wst_upon_start(ctx, r, rsdc_start_job, rsdc_start_rx, s->obj);
    JS_FreeValue(ctx, r);
    return i < 0 ? -1 : 0;
}

JSValue wst_rs_create(JSContext* ctx, JSValueConst start_value, SAlgo* pull, SAlgo* cancel, double hwm,
    JSValueConst size_fn, int size_set, SRS** out)
{
    SRS* s;
    JSValue obj = wst_rs_new(ctx, JS_UNDEFINED, &s);
    if (JS_IsException(obj)) {
        sa_clear(ctx, pull);
        sa_clear(ctx, cancel);
        return obj;
    }
    if (wst_rsdc_setup(ctx, s, JS_UNDEFINED, JS_UNDEFINED, start_value, pull, cancel, hwm, size_fn, size_set) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (out)
        *out = s;
    return obj;
}

SRS* wst_rs_of(JSContext* ctx, JSValueConst v, const char* what)
{
    SRS* s = wst_opaque(v, WSTC_RS);
    if (!s)
        wst_throw(ctx, 0, "%s is not a ReadableStream", what);
    return s;
}

static JSValue rs_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    JSValueConst src_in = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst strat = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue src = JS_NULL, fcancel = JS_UNDEFINED, fpull = JS_UNDEFINED, fstart = JS_UNDEFINED;
    JSValue type = JS_UNDEFINED, size = JS_UNDEFINED, obj = JS_UNDEFINED, aacs_v = JS_UNDEFINED, proto;
    double hwm = 0, h, aacs = 0;
    int hwm_set, aacs_set = 0, bytes = 0, owning = 0;
    SAlgo ap, ac;
    SRS* s;

    WST_NEED_NEW(ctx, nt);
    if (!JS_IsUndefined(src_in) && !JS_IsObject(src_in))
        return wst_throw(ctx, 0, "ReadableStream: underlyingSource must be an object");
    if (wst_strategy(ctx, strat, "strategy", &hwm, &hwm_set, &size) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(src_in))
        src = JS_DupValue(ctx, src_in);
    aacs_v = wst_dict_get(ctx, src, WSA_autoAllocateChunkSize);
    if (JS_IsException(aacs_v))
        goto fail;
    if (!JS_IsUndefined(aacs_v)) {
        if (wst_enforce_ull(ctx, aacs_v, "autoAllocateChunkSize", &aacs) < 0)
            goto fail;
        aacs_set = 1;
    }
    if (wst_cb_member(ctx, src, WSA_cancel, "underlyingSource", &fcancel) < 0
        || wst_cb_member(ctx, src, WSA_pull, "underlyingSource", &fpull) < 0
        || wst_cb_member(ctx, src, WSA_start, "underlyingSource", &fstart) < 0)
        goto fail;
    type = wst_dict_get(ctx, src, WSA_type);
    if (JS_IsException(type))
        goto fail;
    if (!JS_IsUndefined(type)) {
        const char* t = JS_ToCString(ctx, type);
        if (!t)
            goto fail;
        bytes = strcmp(t, "bytes") == 0;
        owning = strcmp(t, "owning") == 0;
        JS_FreeCString(ctx, t);
        if (!bytes && !owning) {
            wst_throw(ctx, 0, "ReadableStream: type must be \"bytes\", \"owning\" or undefined");
            goto fail;
        }
    }
    proto = wst_proto_from_target(ctx, nt);
    if (JS_IsException(proto))
        goto fail;
    obj = wst_rs_new(ctx, proto, &s);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    sa_init(&ap);
    sa_init(&ac);
    if (bytes) {
        if (!JS_IsUndefined(size)) {
            wst_throw(ctx, 1, "the strategy for a byte stream cannot have a size function");
            goto fail;
        }
        if (wst_extract_hwm(ctx, hwm, hwm_set, 0, &h) < 0)
            goto fail;
        if (aacs_set && aacs == 0) {
            wst_throw(ctx, 0, "autoAllocateChunkSize must be greater than 0");
            goto fail;
        }
        sa_set_js(ctx, &ap, fpull, src);
        sa_set_js(ctx, &ac, fcancel, src);
        if (wst_rbsc_setup(ctx, s, fstart, src, JS_UNDEFINED, &ap, &ac, h, aacs, aacs_set) < 0)
            goto fail;
    } else {
        if (wst_extract_hwm(ctx, hwm, hwm_set, 1, &h) < 0)
            goto fail;
        s->owning = owning;
        sa_set_js(ctx, &ap, fpull, src);
        sa_set_js(ctx, &ac, fcancel, src);
        if (wst_rsdc_setup(ctx, s, fstart, src, JS_UNDEFINED, &ap, &ac, h, size, !JS_IsUndefined(size)) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, fcancel);
    JS_FreeValue(ctx, fpull);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, size);
    JS_FreeValue(ctx, aacs_v);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, fcancel);
    JS_FreeValue(ctx, fpull);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, size);
    JS_FreeValue(ctx, aacs_v);
    return JS_EXCEPTION;
}

static JSValue rs_get_locked(JSContext* ctx, JSValueConst this_val)
{
    SRS* s = wst_rs_of(ctx, this_val, "this");
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, wst_rs_locked(s));
}

static JSValue rs_cancel_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_opaque(this_val, WSTC_RS);
    if (!s)
        return wst_reject_err(ctx, 0, "this is not a ReadableStream");
    if (wst_rs_locked(s))
        return wst_reject_err(ctx, 0, "cannot cancel a locked ReadableStream");
    return wst_rs_cancel(ctx, s, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue rs_get_reader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_rs_of(ctx, this_val, "this");
    JSValue d, mode;
    const char* m;
    int ok;
    if (!s)
        return JS_EXCEPTION;
    if (wst_to_dict(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "options", &d) < 0)
        return JS_EXCEPTION;
    mode = wst_dict_get(ctx, d, WSA_mode);
    JS_FreeValue(ctx, d);
    if (JS_IsException(mode))
        return mode;
    if (JS_IsUndefined(mode))
        return wst_rs_acquire_reader(ctx, s, 0, NULL);
    m = JS_ToCString(ctx, mode);
    JS_FreeValue(ctx, mode);
    if (!m)
        return JS_EXCEPTION;
    ok = strcmp(m, "byob") == 0;
    JS_FreeCString(ctx, m);
    if (!ok)
        return wst_throw(ctx, 0, "getReader: mode must be \"byob\" or undefined");
    return wst_rs_acquire_reader(ctx, s, 1, NULL);
}

static JSValue rs_tee_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_rs_of(ctx, this_val, "this");
    JSValue b[2], arr;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (wst_rs_tee(ctx, s, b) < 0)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, b[0]);
        JS_FreeValue(ctx, b[1]);
        return arr;
    }
    JS_SetPropertyUint32(ctx, arr, 0, b[0]);
    JS_SetPropertyUint32(ctx, arr, 1, b[1]);
    return arr;
}

typedef struct SRSIt {
    JSValue obj;
    JSValue reader;
    SReader* r;
    int prevent_cancel;
    JSValue ongoing;
    int finished;
} SRSIt;

static void rsit_finalizer(JSRuntime* rt, JSValue val)
{
    SRSIt* it = wst_opaque(val, WSTC_RSIT);
    if (!it)
        return;
    JS_FreeValueRT(rt, it->reader);
    JS_FreeValueRT(rt, it->ongoing);
    js_free_rt(rt, it);
}

static void rsit_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SRSIt* it = wst_opaque(val, WSTC_RSIT);
    if (!it)
        return;
    JS_MarkValue(rt, it->reader, mf);
    JS_MarkValue(rt, it->ongoing, mf);
}

static JSValue rsit_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
static JSValue rsit_return(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
static JSValue rsit_proto(JSContext* ctx);

static JSValue rs_values(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_rs_of(ctx, this_val, "this");
    JSValue d, pc, robj, obj, ip;
    SReader* r;
    SRSIt* it;
    int prevent;
    if (!s)
        return JS_EXCEPTION;
    if (wst_to_dict(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "options", &d) < 0)
        return JS_EXCEPTION;
    pc = wst_dict_get(ctx, d, WSA_preventCancel);
    JS_FreeValue(ctx, d);
    if (JS_IsException(pc))
        return pc;
    prevent = JS_ToBool(ctx, pc);
    JS_FreeValue(ctx, pc);
    ip = rsit_proto(ctx);
    if (JS_IsException(ip))
        return ip;
    robj = wst_rs_acquire_reader(ctx, s, 0, &r);
    if (JS_IsException(robj))
        return robj;
    obj = wst_new_obj(ctx, WSTC_RSIT, ip, sizeof(SRSIt), (void**)&it);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, robj);
        return obj;
    }
    it->reader = robj;
    it->r = r;
    it->prevent_cancel = prevent;
    it->ongoing = JS_NULL;
    return obj;
}

static const JSCFunctionListEntry rsit_proto_funcs[] = {
    WST_FN("next", 0, rsit_next),
    WST_FN("return", 1, rsit_return),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "ReadableStream AsyncIterator", JS_PROP_CONFIGURABLE),
};

static JSValue rsit_proto(JSContext* ctx)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    JSValue aip, fnproto, tmp, proto;
    if (!JS_IsUndefined(w->rs_iter_proto))
        return w->rs_iter_proto;
    fnproto = JS_Eval(ctx, "(async function* () {}).prototype", 33, "<wst>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fnproto))
        return fnproto;
    tmp = JS_GetPrototype(ctx, fnproto);
    aip = JS_GetPrototype(ctx, tmp);
    JS_FreeValue(ctx, tmp);
    JS_FreeValue(ctx, fnproto);
    proto = JS_NewObjectProto(ctx, aip);
    JS_FreeValue(ctx, aip);
    if (JS_IsException(proto))
        return proto;
    JS_SetPropertyFunctionList(ctx, proto, rsit_proto_funcs, countof(rsit_proto_funcs));
    w->rs_iter_proto = proto;
    return proto;
}

static void rri_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValue a = JS_NewArray(ctx);
    if (!JS_IsException(a) && JS_SetPropertyUint32(ctx, a, 0, JS_DupValue(ctx, chunk)) < 0) {
        JS_FreeValue(ctx, a);
        a = JS_EXCEPTION;
    }
    dyn_settle_value(ctx, rr->a, a);
    JS_FreeValue(ctx, a);
}

static void rri_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    SReader* r = wst_opaque(rr->b, WSTC_RSDR);
    (void)chunk;
    wst_reader_release(ctx, r);
    dyn_settle(ctx, rr->a, JS_NULL, 0);
}

static void rri_error(JSContext* ctx, SReadReq* rr, JSValueConst e)
{
    SReader* r = wst_opaque(rr->b, WSTC_RSDR);
    wst_reader_release(ctx, r);
    dyn_settle(ctx, rr->a, e, 1);
}

static const SRROps rr_iter_ops = { rri_chunk, rri_close, rri_error };

static JSValue rsit_next_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SRSIt* it = wst_opaque(data, WSTC_RSIT);
    JSValue v, r;
    (void)magic;
    JS_FreeValue(ctx, it->ongoing);
    it->ongoing = JS_NULL;
    if (is_reject) {
        it->finished = 1;
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    }
    if (JS_IsNull(arg)) {
        it->finished = 1;
        return wst_iter_result(ctx, JS_UNDEFINED, 1);
    }
    v = JS_GetPropertyUint32(ctx, arg, 0);
    r = wst_iter_result(ctx, v, 0);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue rsit_next_steps(JSContext* ctx, SRSIt* it)
{
    SReadReq rr;
    JSValue d, q, o;
    if (it->finished) {
        o = wst_iter_result(ctx, JS_UNDEFINED, 1);
        q = JS_PromiseResolveValue(ctx, o);
        JS_FreeValue(ctx, o);
        return q;
    }
    if (!it->r->s)
        return wst_reject_err(ctx, 0, "the iterator's reader was released");
    d = JS_NewPromisePending(ctx);
    if (JS_IsException(d))
        return d;
    rr.ops = &rr_iter_ops;
    rr.a = JS_DupValue(ctx, d);
    rr.b = JS_DupValue(ctx, it->reader);
    wst_reader_read(ctx, it->r, &rr);
    q = wst_then(ctx, d, rsit_next_done, it->obj, 0);
    JS_FreeValue(ctx, d);
    return q;
}

static JSValue rsit_next_chain(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    (void)is_reject;
    (void)arg;
    return rsit_next_steps(ctx, wst_opaque(data, WSTC_RSIT));
}

static JSValue rsit_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRSIt* it = wst_opaque(this_val, WSTC_RSIT);
    JSValue n;
    (void)argc;
    (void)argv;
    if (!it)
        return wst_reject_err(ctx, 0, "next() called on an object that is not a ReadableStream async iterator");
    if (!JS_IsNull(it->ongoing))
        n = wst_then(ctx, it->ongoing, rsit_next_chain, it->obj, 0);
    else
        n = rsit_next_steps(ctx, it);
    if (JS_IsException(n))
        return n;
    JS_FreeValue(ctx, it->ongoing);
    it->ongoing = n;
    return JS_DupValue(ctx, n);
}

static JSValue rsit_return_steps(JSContext* ctx, SRSIt* it, JSValueConst value)
{
    JSValue o, q, robj;
    SReader* r = it->r;
    if (it->finished || (it->finished = 1, !r->s)) {
        o = wst_iter_result(ctx, value, 1);
        q = JS_PromiseResolveValue(ctx, o);
        JS_FreeValue(ctx, o);
        return q;
    }
    robj = JS_DupValue(ctx, it->reader);
    if (!it->prevent_cancel) {
        q = wst_rs_cancel(ctx, r->s, value);
        wst_reader_release(ctx, r);
    } else {
        wst_reader_release(ctx, r);
        q = wst_resolved_undef(ctx);
    }
    JS_FreeValue(ctx, robj);
    return q;
}

static JSValue rsit_return_chain(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue its = JS_GetPropertyUint32(ctx, data, 0), v = JS_GetPropertyUint32(ctx, data, 1), r;
    (void)magic;
    (void)is_reject;
    (void)arg;
    r = rsit_return_steps(ctx, wst_opaque(its, WSTC_RSIT), v);
    JS_FreeValue(ctx, its);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue rsit_return_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue v, r;
    (void)magic;
    if (is_reject)
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    v = JS_GetPropertyUint32(ctx, data, 1);
    r = wst_iter_result(ctx, v, 1);
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue rsit_return(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRSIt* it = wst_opaque(this_val, WSTC_RSIT);
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue pair, n, q;
    if (!it)
        return wst_reject_err(ctx, 0, "return() called on an object that is not a ReadableStream async iterator");
    pair = JS_NewArray(ctx);
    if (JS_IsException(pair))
        return pair;
    JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, it->obj));
    JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, value));
    if (!JS_IsNull(it->ongoing))
        n = wst_then(ctx, it->ongoing, rsit_return_chain, pair, 0);
    else
        n = rsit_return_steps(ctx, it, value);
    if (JS_IsException(n)) {
        JS_FreeValue(ctx, pair);
        return n;
    }
    JS_FreeValue(ctx, it->ongoing);
    it->ongoing = n;
    q = wst_then(ctx, n, rsit_return_done, pair, 0);
    JS_FreeValue(ctx, pair);
    return q;
}

static SReader* reader_this(JSValueConst v, int byob)
{
    return wst_opaque(v, byob ? WSTC_RSBR : WSTC_RSDR);
}

static const char* const reader_names[2] = { "ReadableStreamDefaultReader", "ReadableStreamBYOBReader" };

static JSValue reader_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv, int byob)
{
    SRS* s;
    SReader* r;
    JSValue obj, proto;
    WST_NEED_NEW(ctx, nt);
    proto = wst_proto_from_target(ctx, nt);
    if (JS_IsException(proto))
        return proto;
    s = wst_rs_of(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "stream");
    if (!s) {
        JS_FreeValue(ctx, proto);
        return JS_EXCEPTION;
    }
    obj = reader_alloc(ctx, byob, proto, &r);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    if (reader_setup(ctx, r, s) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue rsdr_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return reader_ctor(ctx, nt, argc, argv, 0);
}

static JSValue rsbr_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    return reader_ctor(ctx, nt, argc, argv, 1);
}

static JSValue reader_get_closed(JSContext* ctx, JSValueConst this_val, int magic)
{
    SReader* r = reader_this(this_val, magic);
    char buf[64];
    if (!r) {
        snprintf(buf, sizeof(buf), "this is not a %s", reader_names[magic]);
        return wst_reject_err(ctx, 0, buf);
    }
    return JS_DupValue(ctx, r->closed.p);
}

static JSValue reader_cancel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    SReader* r = reader_this(this_val, magic);
    char buf[64];
    if (!r) {
        snprintf(buf, sizeof(buf), "this is not a %s", reader_names[magic]);
        return wst_reject_err(ctx, 0, buf);
    }
    if (!r->s)
        return wst_reject_err(ctx, 0, "the reader has no stream (lock released)");
    return wst_rs_cancel(ctx, r->s, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue reader_release_lock(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    SReader* r = reader_this(this_val, magic);
    (void)argc;
    (void)argv;
    if (!r)
        return wst_throw(ctx, 0, "this is not a %s", reader_names[magic]);
    if (r->s)
        wst_reader_release(ctx, r);
    return JS_UNDEFINED;
}

static JSValue rsdr_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SReader* r = reader_this(this_val, 0);
    (void)argc;
    (void)argv;
    if (!r)
        return wst_reject_err(ctx, 0, "this is not a ReadableStreamDefaultReader");
    if (!r->s)
        return wst_reject_err(ctx, 0, "the reader has no stream (lock released)");
    return wst_reader_read_promise(ctx, r);
}

static SRSDC* rsdc_this(JSContext* ctx, JSValueConst v)
{
    SRSDC* c = wst_opaque(v, WSTC_RSDC);
    if (!c)
        wst_throw(ctx, 0, "this is not a ReadableStreamDefaultController");
    return c;
}

static JSValue rsdc_get_desired(JSContext* ctx, JSValueConst this_val)
{
    SRSDC* c = rsdc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return wst_rsdc_desired(ctx, c);
}

static JSValue rsdc_close_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRSDC* c = rsdc_this(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (!rsdc_can_close_or_enqueue(c))
        return wst_throw(ctx, 0, "the stream is not in a state that permits close");
    wst_rsdc_close(ctx, c);
    return JS_UNDEFINED;
}

static JSValue rsdc_transfer_list(JSContext* ctx, JSValueConst opts, int* empty)
{
    JSValue t, n;
    int64_t len = 0;
    *empty = 1;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return wst_throw(ctx, 0, "enqueue: options must be an object");
    t = JS_GetPropertyStr(ctx, opts, "transfer");
    if (JS_IsException(t) || JS_IsUndefined(t))
        return t;
    if (!JS_IsObject(t)) {
        JS_FreeValue(ctx, t);
        return wst_throw(ctx, 0, "enqueue: options.transfer must be a sequence of objects");
    }
    n = JS_GetPropertyStr(ctx, t, "length");
    if (JS_IsException(n) || JS_ToInt64(ctx, &len, n) < 0) {
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, t);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, n);
    *empty = len == 0;
    return t;
}

static JSValue rsdc_enqueue_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRSDC* c = rsdc_this(ctx, this_val);
    JSValue chunk = JS_UNDEFINED;
    int r;
    if (!c)
        return JS_EXCEPTION;
    if (argc > 1) {
        int empty;
        JSValue tl = rsdc_transfer_list(ctx, argv[1], &empty);
        if (JS_IsException(tl))
            return tl;
        if (!c->s->owning && !empty) {
            JS_FreeValue(ctx, tl);
            return wst_throw(ctx, 0, "enqueue: a transfer list needs a stream of type \"owning\"");
        }
        if (c->s->owning) {
            JSValue g = JS_GetGlobalObject(ctx), sc = JS_GetPropertyStr(ctx, g, "structuredClone"), o, a[2];
            JS_FreeValue(ctx, g);
            o = JS_NewObject(ctx);
            if (!JS_IsUndefined(tl))
                JS_SetPropertyStr(ctx, o, "transfer", JS_DupValue(ctx, tl));
            a[0] = argv[0];
            a[1] = o;
            chunk = JS_IsException(sc) ? JS_EXCEPTION : JS_Call(ctx, sc, JS_UNDEFINED, 2, (JSValueConst*)a);
            JS_FreeValue(ctx, sc);
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, tl);
            if (JS_IsException(chunk)) {
                JSValue e;
                if (JS_HasUncatchableException(ctx))
                    return JS_EXCEPTION;
                e = JS_GetException(ctx);
                wst_rsdc_error(ctx, c, e);
                return JS_Throw(ctx, e);
            }
        } else
            JS_FreeValue(ctx, tl);
    }
    if (!rsdc_can_close_or_enqueue(c)) {
        JS_FreeValue(ctx, chunk);
        return wst_throw(ctx, 0, "the stream is not in a state that permits enqueue");
    }
    r = wst_rsdc_enqueue(ctx, c, c->s->owning && argc > 1 ? chunk : argc > 0 ? argv[0] : JS_UNDEFINED);
    JS_FreeValue(ctx, chunk);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue rsdc_error_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRSDC* c = rsdc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    wst_rsdc_error(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry rs_proto_funcs[] = {
    WST_GET("locked", rs_get_locked),
    WST_FN("cancel", 0, rs_cancel_m),
    WST_FN("getReader", 0, rs_get_reader),
    WST_FN("pipeThrough", 1, wst_rs_pipe_through_m),
    WST_FN("pipeTo", 1, wst_rs_pipe_to_m),
    WST_FN("tee", 0, rs_tee_m),
    WST_FN("values", 0, rs_values),
    WST_TAG("ReadableStream"),
};

static const JSCFunctionListEntry rs_static_funcs[] = {
    WST_FN("from", 1, wst_rs_from_m),
};

static const JSCFunctionListEntry rsdr_proto_funcs[] = {
    { "closed", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, 0, .u = { .getset = { .get = { .getter_magic = reader_get_closed }, .set = { .setter_magic = NULL } } } },
    { "cancel", JS_PROP_C_W_E, JS_DEF_CFUNC, 0, .u = { .func = { 0, JS_CFUNC_generic_magic, { .generic_magic = reader_cancel } } } },
    WST_FN("read", 0, rsdr_read),
    { "releaseLock", JS_PROP_C_W_E, JS_DEF_CFUNC, 0, .u = { .func = { 0, JS_CFUNC_generic_magic, { .generic_magic = reader_release_lock } } } },
    WST_TAG("ReadableStreamDefaultReader"),
};

static const JSCFunctionListEntry rsbr_proto_funcs[] = {
    { "closed", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, 1, .u = { .getset = { .get = { .getter_magic = reader_get_closed }, .set = { .setter_magic = NULL } } } },
    { "cancel", JS_PROP_C_W_E, JS_DEF_CFUNC, 1, .u = { .func = { 0, JS_CFUNC_generic_magic, { .generic_magic = reader_cancel } } } },
    WST_FN("read", 1, wst_byob_read_m),
    { "releaseLock", JS_PROP_C_W_E, JS_DEF_CFUNC, 1, .u = { .func = { 0, JS_CFUNC_generic_magic, { .generic_magic = reader_release_lock } } } },
    WST_TAG("ReadableStreamBYOBReader"),
};

static const JSCFunctionListEntry rsdc_proto_funcs[] = {
    WST_GET("desiredSize", rsdc_get_desired),
    WST_FN("close", 0, rsdc_close_m),
    WST_FN("enqueue", 0, rsdc_enqueue_m),
    WST_FN("error", 0, rsdc_error_m),
    WST_TAG("ReadableStreamDefaultController"),
};


int wst_init_rs(JSContext* ctx, JSValue* ctors)
{
    static const WstClassSpec specs[] = {
        { WSTC_RS, "ReadableStream", rs_ctor, 0, rs_proto_funcs, countof(rs_proto_funcs), rs_static_funcs, countof(rs_static_funcs), rs_finalizer, rs_mark },
        { WSTC_RSDR, "ReadableStreamDefaultReader", rsdr_ctor, 1, rsdr_proto_funcs, countof(rsdr_proto_funcs), NULL, 0, reader_finalizer, reader_mark },
        { WSTC_RSBR, "ReadableStreamBYOBReader", rsbr_ctor, 1, rsbr_proto_funcs, countof(rsbr_proto_funcs), NULL, 0, reader_finalizer, reader_mark },
        { WSTC_RSDC, "ReadableStreamDefaultController", wst_illegal_ctor, 0, rsdc_proto_funcs, countof(rsdc_proto_funcs), NULL, 0, rsdc_finalizer, rsdc_mark },
    };
    static const WstClassSpec itspec = { WSTC_RSIT, "ReadableStreamAsyncIterator", NULL, 0, NULL, 0, NULL, 0, rsit_finalizer, rsit_mark };
    JSValue proto, values, tmp;
    size_t i;

    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &ctors[specs[i].k]) < 0)
            return -1;
    if (JS_SetClassReaction(JS_GetRuntime(ctx), wst_cid[WSTC_RS], rs_pull_done) < 0)
        return -1;
    proto = wst_proto(ctx, WSTC_RS);
    values = JS_GetPropertyStr(ctx, proto, "values");
    {
        JSValue g = JS_GetGlobalObject(ctx), sym = JS_GetPropertyStr(ctx, g, "Symbol"), ai = JS_IsException(sym) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sym, "asyncIterator");
        JSAtom at = JS_IsException(ai) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, ai);
        int r = -1;
        if (at != JS_ATOM_NULL && !JS_IsException(values))
            r = JS_DefinePropertyValue(ctx, proto, at, JS_DupValue(ctx, values), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_FreeValue(ctx, values);
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, ai);
        JS_FreeValue(ctx, sym);
        JS_FreeValue(ctx, g);
        if (r < 0) {
            JS_FreeValue(ctx, proto);
            return -1;
        }
    }
    JS_FreeValue(ctx, proto);
    if (wst_register(ctx, &itspec, &tmp) < 0)
        return -1;
    return 0;
}
