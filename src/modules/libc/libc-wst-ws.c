#include "libc-wst.h"

static void ws_start_erroring(JSContext* ctx, SWS* s, JSValueConst reason);
static void ws_finish_erroring(JSContext* ctx, SWS* s);
static void wsdc_advance(JSContext* ctx, SWSDC* c);
static void wsdc_clear_algorithms(JSContext* ctx, SWSDC* c);

static int sdef_set(SDef* d)
{
    return !JS_IsUndefined(d->p);
}

static void sdef_clear(JSContext* ctx, SDef* d)
{
    JSValue p = d->p;
    sdef_init(d);
    JS_FreeValue(ctx, p);
}

static void sdef_move(SDef* dst, SDef* src)
{
    *dst = *src;
    sdef_init(src);
}

enum { WREQ_NATIVE = 4 };

static void ws_req_settle(JSContext* ctx, SDef* d, JSValueConst v, int is_reject)
{
    if (!(d->st & WREQ_NATIVE)) {
        if (is_reject)
            sdef_reject(ctx, d, v);
        else
            sdef_resolve(ctx, d, v);
        return;
    }
    if (d->st & 3)
        return;
    d->st |= is_reject ? 2 : 1;
    if (JS_IsNull(d->p)) {
        WstRt* w = wst_rt(ctx);
        if (w)
            w->nat_st = d->st & 3;
    } else {
        JS_EnqueueReaction(ctx, d->p, JS_NewInt64(ctx, d->tag), is_reject);
    }
}

static void wsdc_free(JSRuntime* rt, SWSDC* c);
static void wsdc_mark_in(JSRuntime* rt, SWSDC* c, JS_MarkFunc* mf);

static void ws_finalizer(JSRuntime* rt, JSValue val)
{
    SWS* s = wst_opaque(val, WSTC_WS);
    uint32_t i;
    if (!s)
        return;
    if (s->c) {
        wsdc_free(rt, s->c);
        wst_ctl_drop(rt, s->c);
    }
    JS_FreeValueRT(rt, s->stored_error);
    JS_FreeValueRT(rt, s->writer);
    JS_FreeValueRT(rt, s->ctl);
    for (i = 0; i < s->write_reqs.len; i++)
        sdef_free(rt, sq_at(&s->write_reqs, i));
    sq_release(rt, &s->write_reqs);
    sdef_free(rt, &s->in_flight_write);
    if (s->k) {
        sdef_free(rt, &s->k->close_req);
        sdef_free(rt, &s->k->in_flight_close);
        sdef_free(rt, &s->k->pending_abort.d);
        JS_FreeValueRT(rt, s->k->pending_abort.reason);
        sdef_free(rt, &s->k->abort_d);
        js_free_rt(rt, s->k);
    }
    JS_FreeValueRT(rt, s->native);
    js_free_rt(rt, s);
}

static void ws_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SWS* s = wst_opaque(val, WSTC_WS);
    uint32_t i;
    if (!s)
        return;
    JS_MarkValue(rt, s->stored_error, mf);
    JS_MarkValue(rt, s->writer, mf);
    JS_MarkValue(rt, s->ctl, mf);
    if (s->c)
        wsdc_mark_in(rt, s->c, mf);
    for (i = 0; i < s->write_reqs.len; i++)
        sdef_mark(rt, sq_at(&s->write_reqs, i), mf);
    sdef_mark(rt, &s->in_flight_write, mf);
    if (s->k) {
        sdef_mark(rt, &s->k->close_req, mf);
        sdef_mark(rt, &s->k->in_flight_close, mf);
        sdef_mark(rt, &s->k->pending_abort.d, mf);
        JS_MarkValue(rt, s->k->pending_abort.reason, mf);
        sdef_mark(rt, &s->k->abort_d, mf);
    }
    JS_MarkValue(rt, s->native, mf);
}

static void writer_finalizer(JSRuntime* rt, JSValue val)
{
    SWriter* w = wst_opaque(val, WSTC_WSW);
    if (!w)
        return;
    JS_FreeValueRT(rt, w->stream);
    sdef_free(rt, &w->ready);
    sdef_free(rt, &w->closed);
    js_free_rt(rt, w);
}

static void writer_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SWriter* w = wst_opaque(val, WSTC_WSW);
    if (!w)
        return;
    JS_MarkValue(rt, w->stream, mf);
    sdef_mark(rt, &w->ready, mf);
    sdef_mark(rt, &w->closed, mf);
}

static void wsdc_free_queue(JSRuntime* rt, SWSDC* c)
{
    uint32_t i;
    for (i = 0; i < c->q.len; i++)
        JS_FreeValueRT(rt, ((SQV*)sq_at(&c->q, i))->v);
    sq_release(rt, &c->q);
    c->qtotal = 0;
}

static void wsdc_free(JSRuntime* rt, SWSDC* c)
{
    wsdc_free_queue(rt, c);
    JS_FreeValueRT(rt, c->abort_ctl);
    JS_FreeValueRT(rt, c->size_fn);
    sa_free(rt, &c->write);
    sa_free(rt, &c->close);
    sa_free(rt, &c->abort);
}

static void wsdc_finalizer(JSRuntime* rt, JSValue val)
{
    wst_ctl_fin(rt, wst_opaque(val, WSTC_WSDC));
}

static void wsdc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    wst_ctl_mark(rt, wst_opaque(val, WSTC_WSDC), mf);
}

static void wsdc_mark_in(JSRuntime* rt, SWSDC* c, JS_MarkFunc* mf)
{
    uint32_t i;
    for (i = 0; i < c->q.len; i++)
        JS_MarkValue(rt, ((SQV*)sq_at(&c->q, i))->v, mf);
    JS_MarkValue(rt, c->abort_ctl, mf);
    JS_MarkValue(rt, c->size_fn, mf);
    sa_mark(rt, &c->write, mf);
    sa_mark(rt, &c->close, mf);
    sa_mark(rt, &c->abort, mf);
}

JSValue wst_ws_new(JSContext* ctx, JSValueConst proto, SWS** out)
{
    SWS* s;
    JSValue obj = wst_new_obj(ctx, WSTC_WS, proto, sizeof(SWS), (void**)&s);
    if (JS_IsException(obj))
        return obj;
    s->state = WS_WRITABLE;
    s->stored_error = JS_UNDEFINED;
    s->writer = JS_UNDEFINED;
    s->ctl = JS_UNDEFINED;
    sq_init(&s->write_reqs, sizeof(SDef));
    sdef_init(&s->in_flight_write);
    s->native = JS_UNDEFINED;
    *out = s;
    return obj;
}

static SWSCold* ws_k(JSContext* ctx, SWS* s)
{
    SWSCold* k = s->k;
    if (k)
        return k;
    k = js_malloc(ctx, sizeof(*k));
    if (!k)
        return NULL;
    sdef_init(&k->close_req);
    sdef_init(&k->in_flight_close);
    sdef_init(&k->pending_abort.d);
    k->pending_abort.reason = JS_UNDEFINED;
    k->pending_abort.was_already_erroring = 0;
    k->pending_abort.set = 0;
    sdef_init(&k->abort_d);
    s->k = k;
    return k;
}

int wst_ws_locked(SWS* s)
{
    return !JS_IsUndefined(s->writer);
}

int wst_ws_close_queued_or_in_flight(SWS* s)
{
    return s->k && (sdef_set(&s->k->close_req) || sdef_set(&s->k->in_flight_close));
}

int wst_ws_has_op_in_flight(SWS* s)
{
    return sdef_set(&s->in_flight_write) || (s->k && sdef_set(&s->k->in_flight_close));
}

static void writer_ensure_ready_rejected(JSContext* ctx, SWriter* w, JSValueConst e)
{
    if (w->ready.st == 0)
        sdef_reject(ctx, &w->ready, e);
    else
        sdef_new_rejected(ctx, &w->ready, e);
    JS_PromiseMarkHandled(ctx, w->ready.p);
}

static void writer_ensure_closed_rejected(JSContext* ctx, SWriter* w, JSValueConst e)
{
    if (w->closed.st == 0)
        sdef_reject(ctx, &w->closed, e);
    else
        sdef_new_rejected(ctx, &w->closed, e);
    JS_PromiseMarkHandled(ctx, w->closed.p);
}

static void ws_update_backpressure(JSContext* ctx, SWS* s, int bp)
{
    SWriter* w = s->w;
    if (w && bp != s->backpressure) {
        if (bp)
            sdef_new(ctx, &w->ready);
        else
            sdef_resolve(ctx, &w->ready, JS_UNDEFINED);
    }
    s->backpressure = bp;
}

static double wsdc_desired(SWSDC* c)
{
    return c->hwm - c->qtotal;
}

static int wsdc_backpressure(SWSDC* c)
{
    return wsdc_desired(c) <= 0;
}

static void ws_reject_close_and_closed(JSContext* ctx, SWS* s)
{
    if (s->k && sdef_set(&s->k->close_req)) {
        sdef_reject(ctx, &s->k->close_req, s->stored_error);
        sdef_clear(ctx, &s->k->close_req);
    }
    if (s->w) {
        sdef_reject(ctx, &s->w->closed, s->stored_error);
        JS_PromiseMarkHandled(ctx, s->w->closed.p);
    }
}

static void ws_deal_with_rejection(JSContext* ctx, SWS* s, JSValueConst e)
{
    if (s->state == WS_WRITABLE) {
        ws_start_erroring(ctx, s, e);
        return;
    }
    ws_finish_erroring(ctx, s);
}

static void ws_start_erroring(JSContext* ctx, SWS* s, JSValueConst reason)
{
    SWSDC* c = s->c;
    JSValue old = s->stored_error;
    s->state = WS_ERRORING;
    s->stored_error = JS_DupValue(ctx, reason);
    JS_FreeValue(ctx, old);
    if (s->w)
        writer_ensure_ready_rejected(ctx, s->w, reason);
    if (!wst_ws_has_op_in_flight(s) && c->started)
        ws_finish_erroring(ctx, s);
}

static JSValue ws_abort_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SWS* s = wst_opaque(data, WSTC_WS);
    SDef d;
    (void)magic;
    sdef_move(&d, &s->k->abort_d);
    if (is_reject)
        sdef_reject(ctx, &d, arg);
    else
        sdef_resolve(ctx, &d, JS_UNDEFINED);
    sdef_free(JS_GetRuntime(ctx), &d);
    ws_reject_close_and_closed(ctx, s);
    return JS_UNDEFINED;
}

static void ws_finish_erroring(JSContext* ctx, SWS* s)
{
    SWSDC* c = s->c;
    JSValue e, p, reason, pin;
    SAbortReq ar;
    SQ reqs;
    SDef d;

    s->state = WS_ERRORED;
    wsdc_free_queue(JS_GetRuntime(ctx), c);
    e = JS_DupValue(ctx, s->stored_error);
    sq_steal(&reqs, &s->write_reqs);
    sq_init(&s->write_reqs, sizeof(SDef));
    while (reqs.len) {
        sq_shift(&reqs, &d);
        ws_req_settle(ctx, &d, e, 1);
        sdef_free(JS_GetRuntime(ctx), &d);
    }
    sq_release(JS_GetRuntime(ctx), &reqs);
    if (!s->k || !s->k->pending_abort.set) {
        ws_reject_close_and_closed(ctx, s);
        JS_FreeValue(ctx, e);
        return;
    }
    ar = s->k->pending_abort;
    sdef_init(&s->k->pending_abort.d);
    s->k->pending_abort.reason = JS_UNDEFINED;
    s->k->pending_abort.set = 0;
    if (ar.was_already_erroring) {
        sdef_reject(ctx, &ar.d, e);
        sdef_free(JS_GetRuntime(ctx), &ar.d);
        JS_FreeValue(ctx, ar.reason);
        ws_reject_close_and_closed(ctx, s);
        JS_FreeValue(ctx, e);
        return;
    }
    JS_FreeValue(ctx, e);
    pin = JS_DupValue(ctx, s->obj);
    reason = ar.reason;
    p = sa_call(ctx, &c->abort, 1, (JSValueConst*)&reason);
    JS_FreeValue(ctx, reason);
    wsdc_clear_algorithms(ctx, c);
    sdef_free(JS_GetRuntime(ctx), &s->k->abort_d);
    s->k->abort_d = ar.d;
    if (!JS_IsException(p)) {
        JSValue rx = JS_NewPromiseReaction(ctx, ws_abort_rx, s->obj, 0);
        wst_upon(ctx, p, rx);
        JS_FreeValue(ctx, rx);
    }
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, pin);
}

static void ws_finish_in_flight_close(JSContext* ctx, SWS* s)
{
    SWSCold* k = s->k;
    sdef_resolve(ctx, &k->in_flight_close, JS_UNDEFINED);
    sdef_clear(ctx, &k->in_flight_close);
    if (s->state == WS_ERRORING) {
        JSValue old = s->stored_error;
        s->stored_error = JS_UNDEFINED;
        JS_FreeValue(ctx, old);
        if (k->pending_abort.set) {
            sdef_resolve(ctx, &k->pending_abort.d, JS_UNDEFINED);
            sdef_clear(ctx, &k->pending_abort.d);
            JS_FreeValue(ctx, k->pending_abort.reason);
            k->pending_abort.reason = JS_UNDEFINED;
            k->pending_abort.set = 0;
        }
    }
    s->state = WS_CLOSED;
    if (s->w)
        sdef_resolve(ctx, &s->w->closed, JS_UNDEFINED);
}

static void ws_finish_in_flight_close_err(JSContext* ctx, SWS* s, JSValueConst e)
{
    SWSCold* k = s->k;
    sdef_reject(ctx, &k->in_flight_close, e);
    sdef_clear(ctx, &k->in_flight_close);
    if (k->pending_abort.set) {
        sdef_reject(ctx, &k->pending_abort.d, e);
        sdef_clear(ctx, &k->pending_abort.d);
        JS_FreeValue(ctx, k->pending_abort.reason);
        k->pending_abort.reason = JS_UNDEFINED;
        k->pending_abort.set = 0;
    }
    ws_deal_with_rejection(ctx, s, e);
}

static void wsdc_clear_algorithms(JSContext* ctx, SWSDC* c)
{
    JSValue f = c->size_fn;
    sa_clear(ctx, &c->write);
    sa_clear(ctx, &c->close);
    sa_clear(ctx, &c->abort);
    c->size_fn = JS_UNDEFINED;
    c->size_set = 0;
    JS_FreeValue(ctx, f);
}

static void wsdc_error(JSContext* ctx, SWSDC* c, JSValueConst e)
{
    wsdc_clear_algorithms(ctx, c);
    ws_start_erroring(ctx, c->s, e);
}

static void wsdc_error_if_needed(JSContext* ctx, SWSDC* c, JSValueConst e)
{
    if (c->s->state == WS_WRITABLE)
        wsdc_error(ctx, c, e);
}

static JSValue ws_close_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SWS* s = wst_opaque(data, WSTC_WS);
    (void)magic;
    if (is_reject)
        ws_finish_in_flight_close_err(ctx, s, arg);
    else
        ws_finish_in_flight_close(ctx, s);
    return JS_UNDEFINED;
}

static void wsdc_process_close(JSContext* ctx, SWSDC* c)
{
    SWS* s = c->s;
    JSValue v, pin, rx;
    sdef_move(&s->k->in_flight_close, &s->k->close_req);
    v = wst_queue_shift(&c->q, &c->qtotal);
    JS_FreeValue(ctx, v);
    pin = JS_DupValue(ctx, s->obj);
    rx = JS_NewPromiseReaction(ctx, ws_close_rx, s->obj, 0);
    sa_call_upon(ctx, &c->close, 0, NULL, rx);
    JS_FreeValue(ctx, rx);
    wsdc_clear_algorithms(ctx, c);
    JS_FreeValue(ctx, pin);
}

static JSValue ws_write_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SWS* s = wst_opaque(data, WSTC_WS);
    SWSDC* c = s->c;
    JSValue v;
    (void)magic;
    if (is_reject) {
        if (s->state == WS_WRITABLE)
            wsdc_clear_algorithms(ctx, c);
        ws_req_settle(ctx, &s->in_flight_write, arg, 1);
        sdef_clear(ctx, &s->in_flight_write);
        ws_deal_with_rejection(ctx, s, arg);
        return JS_UNDEFINED;
    }
    ws_req_settle(ctx, &s->in_flight_write, JS_UNDEFINED, 0);
    sdef_clear(ctx, &s->in_flight_write);
    if (c->q.len) {
        v = wst_queue_shift(&c->q, &c->qtotal);
        JS_FreeValue(ctx, v);
    }
    if (!wst_ws_close_queued_or_in_flight(s) && s->state == WS_WRITABLE)
        ws_update_backpressure(ctx, s, wsdc_backpressure(c));
    wsdc_advance(ctx, c);
    return JS_UNDEFINED;
}

static void wsdc_process_write(JSContext* ctx, SWSDC* c, JSValueConst chunk)
{
    SWS* s = c->s;
    JSValue pin, args[2];
    sq_shift_t(&s->write_reqs, &s->in_flight_write, sizeof(SDef));
    args[1] = sa_kind(&c->write) != SA_JS ? JS_UNDEFINED : !JS_IsUndefined(s->ctl) ? s->ctl : wst_ws_ctl(ctx, s);
    if (JS_IsException(args[1])) {
        JSValue e = JS_GetException(ctx);
        JS_EnqueueReaction(ctx, s->obj, e, 1);
        JS_FreeValue(ctx, e);
        return;
    }
    pin = JS_DupValue(ctx, s->obj);
    args[0] = JS_DupValue(ctx, chunk);
    sa_call_upon(ctx, &c->write, 2, (JSValueConst*)args, s->obj);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, pin);
}

static void wsdc_advance(JSContext* ctx, SWSDC* c)
{
    SWS* s = c->s;
    SQV* e;
    if (!c->started)
        return;
    if (sdef_set(&s->in_flight_write))
        return;
    if (s->state == WS_ERRORING) {
        ws_finish_erroring(ctx, s);
        return;
    }
    if (c->q.len == 0)
        return;
    e = sq_peek(&c->q);
    if (e->sentinel)
        wsdc_process_close(ctx, c);
    else
        wsdc_process_write(ctx, c, e->v);
}

static JSValue ws_start_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SWS* s = wst_opaque(data, WSTC_WS);
    SWSDC* c = s->c;
    (void)magic;
    c->started = 1;
    if (is_reject)
        ws_deal_with_rejection(ctx, s, arg);
    else
        wsdc_advance(ctx, c);
    return JS_UNDEFINED;
}

static JSValue ws_start_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)argc;
    return ws_start_rx(ctx, argv[0], 0, 0, argv[1]);
}

static JSValue wsdc_abort_ctl(JSContext* ctx, SWSDC* c)
{
    if (JS_IsUndefined(c->abort_ctl)) {
        WstRt* w = wst_rt(ctx);
        JSValue ac;
        if (!w)
            return JS_EXCEPTION;
        ac = JS_CallConstructor(ctx, w->abort_ctor, 0, NULL);
        if (JS_IsException(ac))
            return ac;
        c->abort_ctl = ac;
    }
    return JS_DupValue(ctx, c->abort_ctl);
}

JSValue wst_ws_abort(JSContext* ctx, SWS* s, JSValueConst reason_in)
{
    JSValue ac, fn, r, reason, pin;
    int was = 0;

    if (s->state == WS_CLOSED || s->state == WS_ERRORED)
        return wst_resolved_undef(ctx);
    pin = JS_DupValue(ctx, s->obj);
    ac = wsdc_abort_ctl(ctx, s->c);
    fn = JS_IsException(ac) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, ac, "abort");
    r = JS_IsException(fn) ? JS_EXCEPTION : JS_Call(ctx, fn, ac, 1, &reason_in);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ac);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, pin);
        return r;
    }
    JS_FreeValue(ctx, r);
    if (s->state == WS_CLOSED || s->state == WS_ERRORED) {
        JS_FreeValue(ctx, pin);
        return wst_resolved_undef(ctx);
    }
    if (s->k && s->k->pending_abort.set) {
        r = JS_DupValue(ctx, s->k->pending_abort.d.p);
        JS_FreeValue(ctx, pin);
        return r;
    }
    reason = JS_DupValue(ctx, reason_in);
    if (s->state == WS_ERRORING) {
        was = 1;
        JS_FreeValue(ctx, reason);
        reason = JS_UNDEFINED;
    }
    if (!ws_k(ctx, s) || sdef_new(ctx, &s->k->pending_abort.d) < 0) {
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, pin);
        return JS_EXCEPTION;
    }
    s->k->pending_abort.reason = reason;
    s->k->pending_abort.was_already_erroring = was;
    s->k->pending_abort.set = 1;
    r = JS_DupValue(ctx, s->k->pending_abort.d.p);
    if (!was)
        ws_start_erroring(ctx, s, reason);
    JS_FreeValue(ctx, pin);
    return r;
}

JSValue wst_ws_close(JSContext* ctx, SWS* s)
{
    SQV* e;
    if (s->state == WS_CLOSED || s->state == WS_ERRORED)
        return wst_reject_err(ctx, 0, "the stream is closed or errored");
    if (!ws_k(ctx, s) || sdef_new(ctx, &s->k->close_req) < 0)
        return JS_EXCEPTION;
    if (s->w && s->backpressure && s->state == WS_WRITABLE)
        sdef_resolve(ctx, &s->w->ready, JS_UNDEFINED);
    e = sq_push(ctx, &s->c->q);
    if (!e)
        return JS_EXCEPTION;
    e->v = JS_UNDEFINED;
    e->size = 0;
    e->sentinel = 1;
    {
        JSValue r = JS_DupValue(ctx, s->k->close_req.p);
        wsdc_advance(ctx, s->c);
        return r;
    }
}

JSValue wst_ws_ctl(JSContext* ctx, SWS* s)
{
    return wst_ctl_get(ctx, WSTC_WSDC, s->c, s->obj, &s->ctl);
}

int wst_ws_setup(JSContext* ctx, SWS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* write, SAlgo* close, SAlgo* abort, double hwm, JSValueConst size_fn, int size_set)
{
    SWSDC* c;
    JSValue cobj, r;
    int i;

    c = js_mallocz(ctx, sizeof(SWSDC));
    if (!c)
        return -1;
    c->obj = JS_UNDEFINED;
    c->stream = JS_UNDEFINED;
    c->s = s;
    sq_init(&c->q, sizeof(SQV));
    c->abort_ctl = JS_UNDEFINED;
    c->size_fn = JS_UNDEFINED;
    sa_init(&c->write);
    sa_init(&c->close);
    sa_init(&c->abort);
    s->c = c;
    c->size_fn = JS_DupValue(ctx, size_fn);
    c->size_set = size_set;
    c->hwm = hwm;
    c->write = *write;
    c->close = *close;
    c->abort = *abort;
    sa_init(write);
    sa_init(close);
    sa_init(abort);
    ws_update_backpressure(ctx, s, wsdc_backpressure(c));
    if (JS_IsUndefined(start_fn)) {
        r = JS_DupValue(ctx, start_value);
    } else {
        cobj = wst_ws_ctl(ctx, s);
        if (JS_IsException(cobj))
            return -1;
        r = JS_Call(ctx, start_fn, start_self, 1, (JSValueConst*)&cobj);
        if (JS_IsException(r))
            return -1;
    }
    i = wst_upon_start(ctx, r, ws_start_job, ws_start_rx, s->obj);
    JS_FreeValue(ctx, r);
    return i < 0 ? -1 : 0;
}

JSValue wst_ws_create(JSContext* ctx, SAlgo* write, SAlgo* close, SAlgo* abort, JSValueConst start_value, double hwm,
    JSValueConst size_fn, int size_set, SWS** out)
{
    SWS* s;
    JSValue obj = wst_ws_new(ctx, JS_UNDEFINED, &s);
    if (JS_IsException(obj))
        return obj;
    if (wst_ws_setup(ctx, s, JS_UNDEFINED, JS_UNDEFINED, start_value, write, close, abort, hwm, size_fn, size_set) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (out)
        *out = s;
    return obj;
}

static SWS* ws_of(JSContext* ctx, JSValueConst v, const char* what)
{
    SWS* s = wst_opaque(v, WSTC_WS);
    if (!s)
        wst_throw(ctx, 0, "%s is not a WritableStream", what);
    return s;
}

static JSValue ws_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    JSValueConst sink_in = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst strat = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue sink = JS_NULL, fabort = JS_UNDEFINED, fclose = JS_UNDEFINED, fstart = JS_UNDEFINED, fwrite = JS_UNDEFINED;
    JSValue type = JS_UNDEFINED, size = JS_UNDEFINED, obj = JS_UNDEFINED;
    SAlgo aw, ac, aa;
    double hwm = 0, h;
    int hwm_set;
    SWS* s;

    WST_NEED_NEW(ctx, nt);
    if (!JS_IsUndefined(sink_in) && !JS_IsObject(sink_in))
        return wst_throw(ctx, 0, "WritableStream: underlyingSink must be an object");
    if (wst_strategy(ctx, strat, "strategy", &hwm, &hwm_set, &size) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(sink_in))
        sink = JS_DupValue(ctx, sink_in);
    if (wst_cb_member(ctx, sink, WSA_abort, "underlyingSink", &fabort) < 0
        || wst_cb_member(ctx, sink, WSA_close, "underlyingSink", &fclose) < 0
        || wst_cb_member(ctx, sink, WSA_start, "underlyingSink", &fstart) < 0)
        goto fail;
    type = wst_dict_get(ctx, sink, WSA_type);
    if (JS_IsException(type))
        goto fail;
    if (wst_cb_member(ctx, sink, WSA_write, "underlyingSink", &fwrite) < 0)
        goto fail;
    if (!JS_IsUndefined(type)) {
        wst_throw(ctx, 1, "WritableStream: invalid type is specified");
        goto fail;
    }
    {
        JSValue proto = wst_proto_from_target(ctx, nt);
        if (JS_IsException(proto))
            goto fail;
        obj = wst_ws_new(ctx, proto, &s);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        goto fail;
    if (wst_extract_hwm(ctx, hwm, hwm_set, 1, &h) < 0)
        goto fail;
    sa_init(&aw);
    sa_init(&ac);
    sa_init(&aa);
    sa_set_js(ctx, &aw, fwrite, sink);
    sa_set_js(ctx, &ac, fclose, sink);
    sa_set_js(ctx, &aa, fabort, sink);
    if (wst_ws_setup(ctx, s, fstart, sink, JS_UNDEFINED, &aw, &ac, &aa, h, size, !JS_IsUndefined(size)) < 0)
        goto fail;
    JS_FreeValue(ctx, sink);
    JS_FreeValue(ctx, fabort);
    JS_FreeValue(ctx, fclose);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, fwrite);
    JS_FreeValue(ctx, size);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, sink);
    JS_FreeValue(ctx, fabort);
    JS_FreeValue(ctx, fclose);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, fwrite);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, size);
    return JS_EXCEPTION;
}

static JSValue ws_get_locked(JSContext* ctx, JSValueConst this_val)
{
    SWS* s = ws_of(ctx, this_val, "this");
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, wst_ws_locked(s));
}

static JSValue ws_abort_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWS* s = wst_opaque(this_val, WSTC_WS);
    if (!s)
        return wst_reject_err(ctx, 0, "this is not a WritableStream");
    if (wst_ws_locked(s))
        return wst_reject_err(ctx, 0, "cannot abort a locked WritableStream");
    return wst_ws_abort(ctx, s, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue ws_close_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWS* s = wst_opaque(this_val, WSTC_WS);
    (void)argc;
    (void)argv;
    if (!s)
        return wst_reject_err(ctx, 0, "this is not a WritableStream");
    if (wst_ws_locked(s))
        return wst_reject_err(ctx, 0, "cannot close a locked WritableStream");
    if (wst_ws_close_queued_or_in_flight(s))
        return wst_reject_err(ctx, 0, "cannot close an already-closing stream");
    return wst_ws_close(ctx, s);
}

static int writer_setup(JSContext* ctx, SWriter* w, SWS* s)
{
    if (wst_ws_locked(s)) {
        wst_throw(ctx, 0, "the WritableStream is locked to a writer");
        return -1;
    }
    w->stream = JS_DupValue(ctx, s->obj);
    w->s = s;
    s->writer = JS_DupValue(ctx, w->obj);
    s->w = w;
    switch (s->state) {
    case WS_WRITABLE:
        if (!wst_ws_close_queued_or_in_flight(s) && s->backpressure)
            sdef_new(ctx, &w->ready);
        else
            sdef_new_resolved(ctx, &w->ready, JS_UNDEFINED);
        sdef_new(ctx, &w->closed);
        break;
    case WS_ERRORING:
        sdef_new_rejected(ctx, &w->ready, s->stored_error);
        sdef_new(ctx, &w->closed);
        break;
    case WS_CLOSED:
        sdef_new_resolved(ctx, &w->ready, JS_UNDEFINED);
        sdef_new_resolved(ctx, &w->closed, JS_UNDEFINED);
        break;
    default:
        sdef_new_rejected(ctx, &w->ready, s->stored_error);
        sdef_new_rejected(ctx, &w->closed, s->stored_error);
        break;
    }
    return 0;
}

static JSValue writer_alloc(JSContext* ctx, JSValueConst proto, SWriter** out)
{
    SWriter* w;
    JSValue obj = wst_new_obj(ctx, WSTC_WSW, proto, sizeof(SWriter), (void**)&w);
    if (JS_IsException(obj))
        return obj;
    w->stream = JS_UNDEFINED;
    sdef_init(&w->ready);
    sdef_init(&w->closed);
    *out = w;
    return obj;
}

JSValue wst_ws_acquire_writer(JSContext* ctx, SWS* s, SWriter** out)
{
    SWriter* w;
    JSValue obj;
    if (wst_ws_locked(s))
        return wst_throw(ctx, 0, "the WritableStream is locked to a writer");
    obj = writer_alloc(ctx, JS_UNDEFINED, &w);
    if (JS_IsException(obj))
        return obj;
    if (writer_setup(ctx, w, s) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (out)
        *out = w;
    return obj;
}

static JSValue ws_get_writer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWS* s = ws_of(ctx, this_val, "this");
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    return wst_ws_acquire_writer(ctx, s, NULL);
}

static JSValue writer_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    SWS* s;
    SWriter* w;
    JSValue obj, proto;
    WST_NEED_NEW(ctx, nt);
    proto = wst_proto_from_target(ctx, nt);
    if (JS_IsException(proto))
        return proto;
    s = ws_of(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "stream");
    if (!s) {
        JS_FreeValue(ctx, proto);
        return JS_EXCEPTION;
    }
    obj = writer_alloc(ctx, proto, &w);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    if (writer_setup(ctx, w, s) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

void wst_writer_release(JSContext* ctx, SWriter* w)
{
    SWS* s = w->s;
    JSValue e = wst_err(ctx, 0, "the writer's lock was released"), so, wo;
    writer_ensure_ready_rejected(ctx, w, e);
    writer_ensure_closed_rejected(ctx, w, e);
    JS_FreeValue(ctx, e);
    wo = s->writer;
    so = w->stream;
    s->writer = JS_UNDEFINED;
    s->w = NULL;
    w->stream = JS_UNDEFINED;
    w->s = NULL;
    JS_FreeValue(ctx, so);
    JS_FreeValue(ctx, wo);
}

static JSValue writer_write_impl(JSContext* ctx, SWriter* w, JSValueConst chunk, int native, uint32_t tag)
{
    SWS* s = w->s;
    SWSDC* c = s->c;
    JSValue size, pin, p;
    SDef* d;

    pin = c->size_set ? JS_DupValue(ctx, s->obj) : JS_UNDEFINED;
    if (wst_size_of(ctx, c->size_fn, c->size_set, chunk, &size) < 0) {
        JSValue e;
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, pin);
            return JS_EXCEPTION;
        }
        e = JS_GetException(ctx);
        wsdc_error_if_needed(ctx, c, e);
        JS_FreeValue(ctx, e);
        size = JS_NewInt32(ctx, 1);
    }
    if (w->s != s) {
        JS_FreeValue(ctx, size);
        JS_FreeValue(ctx, pin);
        return native ? JS_NULL : wst_reject_err(ctx, 0, "the writer's lock was released");
    }
    if (native && (s->state != WS_WRITABLE || wst_ws_close_queued_or_in_flight(s))) {
        p = JS_NULL;
        goto done;
    }
    if (s->state == WS_ERRORED) {
        p = wst_rejected(ctx, s->stored_error);
        goto done;
    }
    if (wst_ws_close_queued_or_in_flight(s) || s->state == WS_CLOSED) {
        p = wst_reject_err(ctx, 0, "the stream is closing or closed");
        goto done;
    }
    if (s->state == WS_ERRORING) {
        p = wst_rejected(ctx, s->stored_error);
        goto done;
    }
    d = sq_push_t(ctx, &s->write_reqs, sizeof(SDef));
    if (!d) {
        p = JS_EXCEPTION;
        goto done;
    }
    sdef_init(d);
    if (native) {
        d->p = JS_NULL;
        d->st = WREQ_NATIVE;
        d->tag = tag;
        p = JS_UNDEFINED;
    } else if (sdef_new(ctx, d) < 0) {
        p = JS_EXCEPTION;
        goto done;
    } else {
        p = JS_DupValue(ctx, d->p);
    }
    if (wst_queue_push(ctx, &c->q, &c->qtotal, chunk, size) < 0) {
        JSValue e = JS_GetException(ctx);
        wsdc_error_if_needed(ctx, c, e);
        JS_FreeValue(ctx, e);
        goto done;
    }
    if (!wst_ws_close_queued_or_in_flight(s) && s->state == WS_WRITABLE)
        ws_update_backpressure(ctx, s, wsdc_backpressure(c));
    wsdc_advance(ctx, c);
done:
    JS_FreeValue(ctx, size);
    JS_FreeValue(ctx, pin);
    return p;
}

JSValue wst_writer_write(JSContext* ctx, SWriter* w, JSValueConst chunk)
{
    return writer_write_impl(ctx, w, chunk, 0, 0);
}

static SDef* ws_unarmed_req(SWS* s)
{
    SDef* d;
    if ((s->in_flight_write.st & WREQ_NATIVE) && JS_IsNull(s->in_flight_write.p))
        return &s->in_flight_write;
    if (!s->write_reqs.len)
        return NULL;
    d = sq_at(&s->write_reqs, s->write_reqs.len - 1);
    return (d->st & WREQ_NATIVE) && JS_IsNull(d->p) ? d : NULL;
}

void wst_writer_write_native(JSContext* ctx, SWriter* w, JSValueConst chunk, JSValueConst rx, uint32_t tag)
{
    WstRt* wr = wst_rt(ctx);
    if (!wr)
        return;
    JSValue so = JS_DupValue(ctx, w->s->obj), r;
    SWS* s = w->s;
    SDef* d;
    wr->nat_st = 0;
    r = writer_write_impl(ctx, w, chunk, 1, tag);
    if (JS_IsException(r)) {
        wst_drop_exc(ctx);
        JS_EnqueueReaction(ctx, rx, JS_NewInt64(ctx, tag), 1);
    } else if (JS_IsNull(r)) {
        JS_EnqueueReaction(ctx, rx, JS_NewInt64(ctx, tag), 1);
    } else if ((d = ws_unarmed_req(s))) {
        d->p = JS_DupValue(ctx, rx);
    } else if (wr->nat_st) {
        JS_EnqueueReaction(ctx, rx, JS_NewInt64(ctx, tag), wr->nat_st == 2);
    }
    wr->nat_st = 0;
    JS_FreeValue(ctx, so);
}

JSValue wst_writer_close_with_error_propagation(JSContext* ctx, SWriter* w)
{
    SWS* s = w->s;
    if (wst_ws_close_queued_or_in_flight(s) || s->state == WS_CLOSED)
        return wst_resolved_undef(ctx);
    if (s->state == WS_ERRORED)
        return wst_rejected(ctx, s->stored_error);
    return wst_ws_close(ctx, s);
}

JSValue wst_writer_desired(JSContext* ctx, SWriter* w)
{
    SWS* s = w->s;
    if (s->state == WS_ERRORED || s->state == WS_ERRORING)
        return JS_NULL;
    if (s->state == WS_CLOSED)
        return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, wsdc_desired(s->c));
}

static SWriter* writer_this(JSValueConst v)
{
    return wst_opaque(v, WSTC_WSW);
}

static JSValue writer_get_closed(JSContext* ctx, JSValueConst this_val)
{
    SWriter* w = writer_this(this_val);
    if (!w)
        return wst_reject_err(ctx, 0, "this is not a WritableStreamDefaultWriter");
    return JS_DupValue(ctx, w->closed.p);
}

static JSValue writer_get_ready(JSContext* ctx, JSValueConst this_val)
{
    SWriter* w = writer_this(this_val);
    if (!w)
        return wst_reject_err(ctx, 0, "this is not a WritableStreamDefaultWriter");
    return JS_DupValue(ctx, w->ready.p);
}

static JSValue writer_get_desired(JSContext* ctx, JSValueConst this_val)
{
    SWriter* w = writer_this(this_val);
    if (!w)
        return wst_throw(ctx, 0, "this is not a WritableStreamDefaultWriter");
    if (!w->s)
        return wst_throw(ctx, 0, "the writer's lock was released");
    return wst_writer_desired(ctx, w);
}

static JSValue writer_abort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWriter* w = writer_this(this_val);
    if (!w)
        return wst_reject_err(ctx, 0, "this is not a WritableStreamDefaultWriter");
    if (!w->s)
        return wst_reject_err(ctx, 0, "the writer's lock was released");
    return wst_ws_abort(ctx, w->s, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue writer_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWriter* w = writer_this(this_val);
    (void)argc;
    (void)argv;
    if (!w)
        return wst_reject_err(ctx, 0, "this is not a WritableStreamDefaultWriter");
    if (!w->s)
        return wst_reject_err(ctx, 0, "the writer's lock was released");
    if (wst_ws_close_queued_or_in_flight(w->s))
        return wst_reject_err(ctx, 0, "cannot close an already-closing stream");
    return wst_ws_close(ctx, w->s);
}

static JSValue writer_release_lock(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWriter* w = writer_this(this_val);
    (void)argc;
    (void)argv;
    if (!w)
        return wst_throw(ctx, 0, "this is not a WritableStreamDefaultWriter");
    if (w->s)
        wst_writer_release(ctx, w);
    return JS_UNDEFINED;
}

static JSValue writer_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWriter* w = writer_this(this_val);
    if (!w)
        return wst_reject_err(ctx, 0, "this is not a WritableStreamDefaultWriter");
    if (!w->s)
        return wst_reject_err(ctx, 0, "the writer's lock was released");
    return wst_writer_write(ctx, w, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue wsdc_get_signal(JSContext* ctx, JSValueConst this_val)
{
    SWSDC* c = wst_opaque(this_val, WSTC_WSDC);
    if (!c)
        return wst_throw(ctx, 0, "this is not a WritableStreamDefaultController");
    {
        JSValue ac = wsdc_abort_ctl(ctx, c), sig;
        if (JS_IsException(ac))
            return ac;
        sig = JS_GetPropertyStr(ctx, ac, "signal");
        JS_FreeValue(ctx, ac);
        return sig;
    }
}

static JSValue wsdc_error_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SWSDC* c = wst_opaque(this_val, WSTC_WSDC);
    if (!c)
        return wst_throw(ctx, 0, "this is not a WritableStreamDefaultController");
    if (c->s->state != WS_WRITABLE)
        return JS_UNDEFINED;
    wsdc_error(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

void wst_wsdc_error_if_needed(JSContext* ctx, SWSDC* c, JSValueConst e)
{
    wsdc_error_if_needed(ctx, c, e);
}

void wst_ws_finish_erroring_if_started(JSContext* ctx, SWS* s, JSValueConst e)
{
    ws_deal_with_rejection(ctx, s, e);
}

static const JSCFunctionListEntry ws_proto_funcs[] = {
    WST_GET("locked", ws_get_locked),
    WST_FN("abort", 0, ws_abort_m),
    WST_FN("close", 0, ws_close_m),
    WST_FN("getWriter", 0, ws_get_writer),
    WST_TAG("WritableStream"),
};

static const JSCFunctionListEntry writer_proto_funcs[] = {
    WST_GET("closed", writer_get_closed),
    WST_GET("desiredSize", writer_get_desired),
    WST_GET("ready", writer_get_ready),
    WST_FN("abort", 0, writer_abort),
    WST_FN("close", 0, writer_close),
    WST_FN("releaseLock", 0, writer_release_lock),
    WST_FN("write", 0, writer_write),
    WST_TAG("WritableStreamDefaultWriter"),
};

static const JSCFunctionListEntry wsdc_proto_funcs[] = {
    WST_GET("signal", wsdc_get_signal),
    WST_FN("error", 0, wsdc_error_m),
    WST_TAG("WritableStreamDefaultController"),
};

int wst_init_ws(JSContext* ctx, JSValue* ctors)
{
    static const WstClassSpec specs[] = {
        { WSTC_WS, "WritableStream", ws_ctor, 0, ws_proto_funcs, countof(ws_proto_funcs), NULL, 0, ws_finalizer, ws_mark },
        { WSTC_WSW, "WritableStreamDefaultWriter", writer_ctor, 1, writer_proto_funcs, countof(writer_proto_funcs), NULL, 0, writer_finalizer, writer_mark },
        { WSTC_WSDC, "WritableStreamDefaultController", wst_illegal_ctor, 0, wsdc_proto_funcs, countof(wsdc_proto_funcs), NULL, 0, wsdc_finalizer, wsdc_mark },
    };
    size_t i;
    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &ctors[specs[i].k]) < 0)
            return -1;
    if (JS_SetClassReaction(JS_GetRuntime(ctx), wst_cid[WSTC_WS], ws_write_rx) < 0)
        return -1;
    return 0;
}
