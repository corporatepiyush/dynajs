#include "libc-wst.h"

enum { TSX_ABORT, TSX_CLOSE, TSX_CANCEL };

static void ts_finalizer(JSRuntime* rt, JSValue val)
{
    STS* t = wst_opaque(val, WSTC_TS);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->readable);
    JS_FreeValueRT(rt, t->writable);
    sdef_free(rt, &t->bp_change);
    JS_FreeValueRT(rt, t->bp_waiter);
    JS_FreeValueRT(rt, t->ctl);
    js_free_rt(rt, t);
}

static void ts_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    STS* t = wst_opaque(val, WSTC_TS);
    if (!t)
        return;
    JS_MarkValue(rt, t->readable, mf);
    JS_MarkValue(rt, t->writable, mf);
    sdef_mark(rt, &t->bp_change, mf);
    JS_MarkValue(rt, t->bp_waiter, mf);
    JS_MarkValue(rt, t->ctl, mf);
}

static void tsdc_finalizer(JSRuntime* rt, JSValue val)
{
    STSDC* c = wst_opaque(val, WSTC_TSDC);
    if (!c)
        return;
    JS_FreeValueRT(rt, c->stream);
    sa_free(rt, &c->transform);
    sa_free(rt, &c->flush);
    sa_free(rt, &c->cancel);
    sdef_free(rt, &c->finish);
    JS_FreeValueRT(rt, c->finish_reason);
    js_free_rt(rt, c);
}

static void tsdc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    STSDC* c = wst_opaque(val, WSTC_TSDC);
    if (!c)
        return;
    JS_MarkValue(rt, c->stream, mf);
    sa_mark(rt, &c->transform, mf);
    sa_mark(rt, &c->flush, mf);
    sa_mark(rt, &c->cancel, mf);
    sdef_mark(rt, &c->finish, mf);
    JS_MarkValue(rt, c->finish_reason, mf);
}

static void ts_set_backpressure(JSContext* ctx, STS* t, int bp)
{
    if (!JS_IsUndefined(t->bp_change.p)) {
        sdef_resolve(ctx, &t->bp_change, JS_UNDEFINED);
        sdef_free(JS_GetRuntime(ctx), &t->bp_change);
    } else if (!JS_IsUndefined(t->bp_waiter)) {
        JSValue w = t->bp_waiter;
        t->bp_waiter = JS_UNDEFINED;
        JS_EnqueueReaction(ctx, w, JS_UNDEFINED, 0);
        JS_FreeValue(ctx, w);
    }
    t->backpressure = bp;
}

static JSValue ts_bp_change_promise(JSContext* ctx, STS* t)
{
    if (JS_IsUndefined(t->bp_change.p)) {
        if (sdef_new(ctx, &t->bp_change) < 0)
            return JS_EXCEPTION;
        if (!JS_IsUndefined(t->bp_waiter)) {
            JSValue w = t->bp_waiter;
            t->bp_waiter = JS_UNDEFINED;
            JS_PromiseReact(ctx, t->bp_change.p, w);
            JS_FreeValue(ctx, w);
        }
    }
    return t->bp_change.p;
}

static void ts_unblock_write(JSContext* ctx, STS* t)
{
    if (t->backpressure == 1)
        ts_set_backpressure(ctx, t, 0);
}

static void tsdc_clear_algorithms(JSContext* ctx, STSDC* c)
{
    sa_clear(ctx, &c->transform);
    sa_clear(ctx, &c->flush);
    sa_clear(ctx, &c->cancel);
}

static void ts_error_writable_and_unblock(JSContext* ctx, STS* t, JSValueConst e)
{
    tsdc_clear_algorithms(ctx, t->c);
    wst_wsdc_error_if_needed(ctx, t->ws->c, e);
    ts_unblock_write(ctx, t);
}

static void ts_error(JSContext* ctx, STS* t, JSValueConst e)
{
    wst_rsdc_error(ctx, t->rs->c, e);
    ts_error_writable_and_unblock(ctx, t, e);
}

int wst_tsdc_enqueue(JSContext* ctx, STSDC* c, JSValueConst chunk)
{
    STS* t = c->t;
    SRSDC* rc = t->rs->c;
    JSValue pin;
    int bp;
    if (!wst_rsdc_can_close_or_enqueue(rc)) {
        wst_throw(ctx, 0, "the readable side is not in a state that permits enqueue");
        return -1;
    }
    pin = JS_DupValue(ctx, t->obj);
    if (wst_rsdc_enqueue(ctx, rc, chunk) < 0) {
        JSValue e;
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, pin);
            return -1;
        }
        e = JS_GetException(ctx);
        ts_error_writable_and_unblock(ctx, t, e);
        JS_FreeValue(ctx, e);
        JS_Throw(ctx, JS_DupValue(ctx, t->rs->stored_error));
        JS_FreeValue(ctx, pin);
        return -1;
    }
    bp = wst_rsdc_has_backpressure(rc);
    if (bp != t->backpressure)
        ts_set_backpressure(ctx, t, 1);
    JS_FreeValue(ctx, pin);
    return 0;
}

void wst_tsdc_error(JSContext* ctx, STSDC* c, JSValueConst e)
{
    ts_error(ctx, c->t, e);
}

void wst_tsdc_terminate(JSContext* ctx, STSDC* c)
{
    STS* t = c->t;
    JSValue e;
    wst_rsdc_close(ctx, t->rs->c);
    e = wst_err(ctx, 0, "the TransformStream has been terminated");
    ts_error_writable_and_unblock(ctx, t, e);
    JS_FreeValue(ctx, e);
}

static JSValue ts_identity(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    STSDC* c = wst_opaque(data, WSTC_TSDC);
    if (wst_tsdc_enqueue(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED) < 0)
        return wst_reject_exc(ctx);
    return wst_resolved_undef(ctx);
}

static JSValue ts_perform_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    STSDC* c = wst_opaque(data, WSTC_TSDC);
    (void)magic;
    if (!is_reject)
        return JS_DupValue(ctx, arg);
    ts_error(ctx, c->t, arg);
    return JS_Throw(ctx, JS_DupValue(ctx, arg));
}

static JSValue tsdc_perform_transform(JSContext* ctx, STSDC* c, JSValueConst chunk)
{
    JSValueConst args[2];
    JSValue p, q, pin = JS_DupValue(ctx, c->obj);
    args[0] = chunk;
    args[1] = c->obj;
    p = sa_call(ctx, &c->transform, 2, args);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, pin);
        return p;
    }
    q = wst_then(ctx, p, ts_perform_rx, c->obj, 0);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, pin);
    return q;
}

static JSValue ts_write_bp_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue tobj = JS_GetPropertyUint32(ctx, data, 0), chunk = JS_GetPropertyUint32(ctx, data, 1), r;
    STS* t = wst_opaque(tobj, WSTC_TS);
    (void)magic;
    if (is_reject)
        r = JS_Throw(ctx, JS_DupValue(ctx, arg));
    else if (t->ws->state == WS_ERRORING)
        r = JS_Throw(ctx, JS_DupValue(ctx, t->ws->stored_error));
    else
        r = tsdc_perform_transform(ctx, t->c, chunk);
    JS_FreeValue(ctx, tobj);
    JS_FreeValue(ctx, chunk);
    return r;
}

static JSValue ts_tf_done_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    STSDC* c = wst_opaque(data, WSTC_TSDC);
    JSValueConst rx = c->t->ws->obj;
    JSValue pin = JS_DupValue(ctx, c->obj);
    int rc = 0;
    (void)magic;
    if (is_reject) {
        ts_error(ctx, c->t, arg);
        JS_EnqueueReaction(ctx, rx, arg, 1);
    } else if (!JS_IsObject(arg)) {
        JS_EnqueueReaction(ctx, rx, arg, 0);
    } else {
        JSValue q = JS_NewPromisePending(ctx);
        if (!JS_IsException(q)) {
            JS_PromiseReact(ctx, q, rx);
            rc = dyn_settle(ctx, q, arg, 0);
        }
        JS_FreeValue(ctx, q);
    }
    JS_FreeValue(ctx, pin);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue ts_tf_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    STS* t = wst_opaque(data, WSTC_TS);
    return ts_tf_done_rx(ctx, t->ctl, magic, is_reject, arg);
}

static void tsdc_perform_transform_upon(JSContext* ctx, STSDC* c, JSValueConst chunk)
{
    JSValueConst args[2];
    JSValueConst rx = c->t->obj;
    JSValue r, p, pin = JS_DupValue(ctx, c->obj);
    SAlgo* a = &c->transform;
    args[0] = chunk;
    args[1] = c->obj;
    if (sa_kind(a) == SA_C && a->cfn == ts_identity) {
        if (wst_tsdc_enqueue(ctx, c, chunk) < 0) {
            JSValue e;
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, pin);
                return;
            }
            e = JS_GetException(ctx);
            JS_EnqueueReaction(ctx, rx, e, 1);
            JS_FreeValue(ctx, e);
        } else {
            JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
        }
    } else if (sa_kind(a) == SA_JS) {
        JSValue f = JS_DupValue(ctx, a->fn), s = JS_DupValue(ctx, a->self);
        r = JS_Call(ctx, f, s, 2, args);
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, s);
        if (JS_IsException(r)) {
            JSValue e;
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, pin);
                return;
            }
            e = JS_GetException(ctx);
            JS_EnqueueReaction(ctx, rx, e, 1);
            JS_FreeValue(ctx, e);
        } else if (!JS_IsObject(r)) {
            JS_EnqueueReaction(ctx, rx, r, 0);
        } else {
            p = wst_promise_resolved_with(ctx, r);
            if (JS_IsException(p))
                p = wst_reject_exc(ctx);
            if (!JS_IsException(p))
                JS_PromiseReact(ctx, p, rx);
            JS_FreeValue(ctx, p);
        }
        JS_FreeValue(ctx, r);
    } else if (sa_kind(a) == SA_NONE) {
        JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
    } else {
        p = sa_call(ctx, a, 2, args);
        if (!JS_IsException(p))
            JS_PromiseReact(ctx, p, rx);
        JS_FreeValue(ctx, p);
    }
    JS_FreeValue(ctx, pin);
}

static void ts_sink_write(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv, JSValueConst rx)
{
    STS* t = wst_opaque(data, WSTC_TS);
    JSValueConst chunk = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (t->backpressure == 1) {
        JSValue pair = JS_NewArray(ctx), q;
        JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, data));
        JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, chunk));
        q = JS_IsException(ts_bp_change_promise(ctx, t)) ? JS_EXCEPTION : wst_then(ctx, t->bp_change.p, ts_write_bp_rx, pair, 0);
        JS_FreeValue(ctx, pair);
        if (!JS_IsException(q))
            JS_PromiseReact(ctx, q, rx);
        JS_FreeValue(ctx, q);
        return;
    }
    tsdc_perform_transform_upon(ctx, t->c, chunk);
}

static JSValue ts_finish_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    STSDC* c = wst_opaque(data, WSTC_TSDC);
    STS* t = c->t;
    JSValue pin = JS_DupValue(ctx, c->obj);
    if (magic == TSX_CANCEL) {
        SWS* w = t->ws;
        if (is_reject) {
            wst_wsdc_error_if_needed(ctx, w->c, arg);
            ts_unblock_write(ctx, t);
            sdef_reject(ctx, &c->finish, arg);
        } else if (w->state == WS_ERRORED) {
            sdef_reject(ctx, &c->finish, w->stored_error);
        } else {
            wst_wsdc_error_if_needed(ctx, w->c, c->finish_reason);
            ts_unblock_write(ctx, t);
            sdef_resolve(ctx, &c->finish, JS_UNDEFINED);
        }
    } else {
        SRS* r = t->rs;
        if (is_reject) {
            wst_rsdc_error(ctx, r->c, arg);
            sdef_reject(ctx, &c->finish, arg);
        } else if (r->state == RS_ERRORED) {
            sdef_reject(ctx, &c->finish, r->stored_error);
        } else {
            if (magic == TSX_ABORT)
                wst_rsdc_error(ctx, r->c, c->finish_reason);
            else
                wst_rsdc_close(ctx, r->c);
            sdef_resolve(ctx, &c->finish, JS_UNDEFINED);
        }
    }
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue ts_finish(JSContext* ctx, STS* t, int kind, JSValueConst reason)
{
    STSDC* c = t->c;
    JSValue p, rx, pin;
    if (!JS_IsUndefined(c->finish.p))
        return JS_DupValue(ctx, c->finish.p);
    if (sdef_new(ctx, &c->finish) < 0)
        return JS_EXCEPTION;
    pin = JS_DupValue(ctx, c->obj);
    JS_FreeValue(ctx, c->finish_reason);
    c->finish_reason = JS_DupValue(ctx, reason);
    if (kind == TSX_CLOSE) {
        JSValueConst a = c->obj;
        p = sa_call(ctx, &c->flush, 1, &a);
    } else {
        p = sa_call(ctx, &c->cancel, 1, &reason);
    }
    tsdc_clear_algorithms(ctx, c);
    if (!JS_IsException(p)) {
        rx = JS_NewPromiseReaction(ctx, ts_finish_rx, c->obj, kind);
        if (!JS_IsException(rx))
            JS_PromiseReact(ctx, p, rx);
        JS_FreeValue(ctx, rx);
    }
    JS_FreeValue(ctx, p);
    p = JS_DupValue(ctx, c->finish.p);
    JS_FreeValue(ctx, pin);
    return p;
}

static JSValue ts_sink_close(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return ts_finish(ctx, wst_opaque(data, WSTC_TS), TSX_CLOSE, JS_UNDEFINED);
}

static JSValue ts_sink_abort(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    return ts_finish(ctx, wst_opaque(data, WSTC_TS), TSX_ABORT, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue ts_source_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    return ts_finish(ctx, wst_opaque(data, WSTC_TS), TSX_CANCEL, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static void ts_source_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv, JSValueConst rx)
{
    STS* t = wst_opaque(data, WSTC_TS);
    (void)argc;
    (void)argv;
    ts_set_backpressure(ctx, t, 0);
    JS_FreeValue(ctx, t->bp_waiter);
    t->bp_waiter = JS_DupValue(ctx, rx);
}

static JSValue ts_alloc(JSContext* ctx, JSValueConst proto, STS** out)
{
    STS* t;
    JSValue obj = wst_new_obj(ctx, WSTC_TS, proto, sizeof(STS), (void**)&t);
    if (JS_IsException(obj))
        return obj;
    t->readable = JS_UNDEFINED;
    t->writable = JS_UNDEFINED;
    t->backpressure = -1;
    sdef_init(&t->bp_change);
    t->bp_waiter = JS_UNDEFINED;
    t->ctl = JS_UNDEFINED;
    *out = t;
    return obj;
}

static int ts_init(JSContext* ctx, STS* t, JSValueConst start_p, double whwm, JSValueConst wsize, int wsize_set,
    double rhwm, JSValueConst rsize, int rsize_set)
{
    SAlgo a, b, c;
    sa_init(&a);
    sa_init(&b);
    sa_init(&c);
    sa_set_u(ctx, &a, ts_sink_write, t->obj);
    sa_set_c(ctx, &b, ts_sink_close, t->obj);
    sa_set_c(ctx, &c, ts_sink_abort, t->obj);
    t->writable = wst_ws_create(ctx, &a, &b, &c, start_p, whwm, wsize, wsize_set, &t->ws);
    if (JS_IsException(t->writable)) {
        t->writable = JS_UNDEFINED;
        return -1;
    }
    sa_set_u(ctx, &a, ts_source_pull, t->obj);
    sa_set_c(ctx, &b, ts_source_cancel, t->obj);
    t->readable = wst_rs_create(ctx, start_p, &a, &b, rhwm, rsize, rsize_set, &t->rs);
    if (JS_IsException(t->readable)) {
        t->readable = JS_UNDEFINED;
        return -1;
    }
    ts_set_backpressure(ctx, t, 1);
    return 0;
}

static int tsdc_setup(JSContext* ctx, STS* t, SAlgo* transform, SAlgo* flush, SAlgo* cancel)
{
    STSDC* c;
    JSValue cobj = wst_new_obj(ctx, WSTC_TSDC, JS_UNDEFINED, sizeof(STSDC), (void**)&c);
    if (JS_IsException(cobj)) {
        sa_clear(ctx, transform);
        sa_clear(ctx, flush);
        sa_clear(ctx, cancel);
        return -1;
    }
    c->stream = JS_DupValue(ctx, t->obj);
    c->t = t;
    sdef_init(&c->finish);
    c->finish_reason = JS_UNDEFINED;
    if (sa_kind(transform) != SA_NONE) {
        c->transform = *transform;
        sa_init(transform);
    } else {
        sa_init(&c->transform);
        sa_set_c(ctx, &c->transform, ts_identity, cobj);
    }
    c->flush = *flush;
    c->cancel = *cancel;
    sa_init(flush);
    sa_init(cancel);
    t->ctl = cobj;
    t->c = c;
    return 0;
}

JSValue wst_ts_create(JSContext* ctx, SAlgo* transform, SAlgo* flush, SAlgo* cancel, STS** out)
{
    STS* t;
    JSValue obj = ts_alloc(ctx, JS_UNDEFINED, &t), sp;
    if (JS_IsException(obj))
        return obj;
    sp = wst_resolved_undef(ctx);
    if (ts_init(ctx, t, sp, 1, JS_UNDEFINED, 0, 0, JS_UNDEFINED, 0) < 0 || tsdc_setup(ctx, t, transform, flush, cancel) < 0) {
        JS_FreeValue(ctx, sp);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, sp);
    if (out)
        *out = t;
    return obj;
}

static JSValue ts_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    JSValueConst tr_in = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue tr = JS_NULL, wsize = JS_UNDEFINED, rsize = JS_UNDEFINED, fcancel = JS_UNDEFINED, fflush = JS_UNDEFINED;
    JSValue fstart = JS_UNDEFINED, ftransform = JS_UNDEFINED, rtype = JS_UNDEFINED, wtype = JS_UNDEFINED, obj = JS_UNDEFINED, proto;
    double wh = 0, rh = 0, rhwm, whwm;
    int wh_set, rh_set;
    SAlgo at, af, ac;
    SDef start_d;
    STS* t;

    WST_NEED_NEW(ctx, nt);
    sdef_init(&start_d);
    if (!JS_IsUndefined(tr_in) && !JS_IsObject(tr_in))
        return wst_throw(ctx, 0, "TransformStream: transformer must be an object");
    if (wst_strategy(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "writableStrategy", &wh, &wh_set, &wsize) < 0)
        return JS_EXCEPTION;
    if (wst_strategy(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "readableStrategy", &rh, &rh_set, &rsize) < 0)
        goto fail;
    if (!JS_IsUndefined(tr_in))
        tr = JS_DupValue(ctx, tr_in);
    if (wst_cb_member(ctx, tr, WSA_cancel, "transformer", &fcancel) < 0
        || wst_cb_member(ctx, tr, WSA_flush, "transformer", &fflush) < 0)
        goto fail;
    rtype = wst_dict_get(ctx, tr, WSA_readableType);
    if (JS_IsException(rtype))
        goto fail;
    if (wst_cb_member(ctx, tr, WSA_start, "transformer", &fstart) < 0
        || wst_cb_member(ctx, tr, WSA_transform, "transformer", &ftransform) < 0)
        goto fail;
    wtype = wst_dict_get(ctx, tr, WSA_writableType);
    if (JS_IsException(wtype))
        goto fail;
    if (!JS_IsUndefined(rtype)) {
        wst_throw(ctx, 1, "TransformStream: invalid readableType specified");
        goto fail;
    }
    if (!JS_IsUndefined(wtype)) {
        wst_throw(ctx, 1, "TransformStream: invalid writableType specified");
        goto fail;
    }
    if (wst_extract_hwm(ctx, rh, rh_set, 0, &rhwm) < 0 || wst_extract_hwm(ctx, wh, wh_set, 1, &whwm) < 0)
        goto fail;
    if (sdef_new(ctx, &start_d) < 0)
        goto fail;
    proto = wst_proto_from_target(ctx, nt);
    if (JS_IsException(proto))
        goto fail;
    obj = ts_alloc(ctx, proto, &t);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    if (ts_init(ctx, t, start_d.p, whwm, wsize, !JS_IsUndefined(wsize), rhwm, rsize, !JS_IsUndefined(rsize)) < 0)
        goto fail;
    sa_init(&at);
    sa_init(&af);
    sa_init(&ac);
    sa_set_js(ctx, &at, ftransform, tr);
    sa_set_js(ctx, &af, fflush, tr);
    sa_set_js(ctx, &ac, fcancel, tr);
    if (tsdc_setup(ctx, t, &at, &af, &ac) < 0)
        goto fail;
    if (!JS_IsUndefined(fstart)) {
        JSValue r = JS_Call(ctx, fstart, tr, 1, (JSValueConst*)&t->ctl);
        if (JS_IsException(r))
            goto fail;
        sdef_resolve(ctx, &start_d, r);
        JS_FreeValue(ctx, r);
    } else {
        sdef_resolve(ctx, &start_d, JS_UNDEFINED);
    }
    sdef_free(JS_GetRuntime(ctx), &start_d);
    JS_FreeValue(ctx, tr);
    JS_FreeValue(ctx, wsize);
    JS_FreeValue(ctx, rsize);
    JS_FreeValue(ctx, fcancel);
    JS_FreeValue(ctx, fflush);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, ftransform);
    return obj;
fail:
    sdef_free(JS_GetRuntime(ctx), &start_d);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, tr);
    JS_FreeValue(ctx, wsize);
    JS_FreeValue(ctx, rsize);
    JS_FreeValue(ctx, fcancel);
    JS_FreeValue(ctx, fflush);
    JS_FreeValue(ctx, fstart);
    JS_FreeValue(ctx, ftransform);
    JS_FreeValue(ctx, rtype);
    JS_FreeValue(ctx, wtype);
    return JS_EXCEPTION;
}

static JSValue ts_get_side(JSContext* ctx, JSValueConst this_val, int magic)
{
    STS* t = wst_opaque(this_val, WSTC_TS);
    if (!t)
        return wst_throw(ctx, 0, "this is not a TransformStream");
    return JS_DupValue(ctx, magic ? t->writable : t->readable);
}

static STSDC* tsdc_this(JSContext* ctx, JSValueConst v)
{
    STSDC* c = wst_opaque(v, WSTC_TSDC);
    if (!c)
        wst_throw(ctx, 0, "this is not a TransformStreamDefaultController");
    return c;
}

static JSValue tsdc_get_desired(JSContext* ctx, JSValueConst this_val)
{
    STSDC* c = tsdc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return wst_rsdc_desired(ctx, c->t->rs->c);
}

static JSValue tsdc_enqueue_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    STSDC* c = tsdc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    if (wst_tsdc_enqueue(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue tsdc_error_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    STSDC* c = tsdc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    wst_tsdc_error(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

static JSValue tsdc_terminate_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    STSDC* c = tsdc_this(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    wst_tsdc_terminate(ctx, c);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry ts_proto_funcs[] = {
    { "readable", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, 0, .u = { .getset = { .get = { .getter_magic = ts_get_side }, .set = { .setter_magic = NULL } } } },
    { "writable", JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET_MAGIC, 1, .u = { .getset = { .get = { .getter_magic = ts_get_side }, .set = { .setter_magic = NULL } } } },
    WST_TAG("TransformStream"),
};

static const JSCFunctionListEntry tsdc_proto_funcs[] = {
    WST_GET("desiredSize", tsdc_get_desired),
    WST_FN("enqueue", 0, tsdc_enqueue_m),
    WST_FN("error", 0, tsdc_error_m),
    WST_FN("terminate", 0, tsdc_terminate_m),
    WST_TAG("TransformStreamDefaultController"),
};

int wst_init_ts(JSContext* ctx, JSValue* ctors)
{
    static const WstClassSpec specs[] = {
        { WSTC_TS, "TransformStream", ts_ctor, 0, ts_proto_funcs, countof(ts_proto_funcs), NULL, 0, ts_finalizer, ts_mark },
        { WSTC_TSDC, "TransformStreamDefaultController", wst_illegal_ctor, 0, tsdc_proto_funcs, countof(tsdc_proto_funcs), NULL, 0, tsdc_finalizer, tsdc_mark },
    };
    size_t i;
    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &ctors[specs[i].k]) < 0)
            return -1;
    if (JS_SetClassReaction(JS_GetRuntime(ctx), wst_cid[WSTC_TS], ts_tf_done) < 0)
        return -1;
    return 0;
}

JSValue wst_ts_shell(JSContext* ctx, JSValue readable, SRS* rs, JSValue writable, SWS* ws)
{
    STS* t;
    JSValue obj = ts_alloc(ctx, JS_UNDEFINED, &t);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, readable);
        JS_FreeValue(ctx, writable);
        return obj;
    }
    t->readable = readable;
    t->rs = rs;
    t->writable = writable;
    t->ws = ws;
    return obj;
}
