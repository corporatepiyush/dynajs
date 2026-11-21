#include "dyna-nat.h"
#include "libc-wst.h"

typedef struct STee {
    JSValue obj;
    JSValue source;
    SRS* s;
    JSValue reader;
    SReader* r;
    int bytes, reading, read_again, read_again1, read_again2, canceled1, canceled2;
    JSValue reason1, reason2;
    JSValue branch[2];
    SRS* b[2];
    SDef cancel_d;
} STee;

static void tee_finalizer(JSRuntime* rt, JSValue val)
{
    STee* t = wst_opaque(val, WSTC_TEE);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->source);
    JS_FreeValueRT(rt, t->reader);
    JS_FreeValueRT(rt, t->reason1);
    JS_FreeValueRT(rt, t->reason2);
    JS_FreeValueRT(rt, t->branch[0]);
    JS_FreeValueRT(rt, t->branch[1]);
    sdef_free(rt, &t->cancel_d);
    js_free_rt(rt, t);
}

static void tee_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    STee* t = wst_opaque(val, WSTC_TEE);
    if (!t)
        return;
    JS_MarkValue(rt, t->source, mf);
    JS_MarkValue(rt, t->reader, mf);
    JS_MarkValue(rt, t->reason1, mf);
    JS_MarkValue(rt, t->reason2, mf);
    JS_MarkValue(rt, t->branch[0], mf);
    JS_MarkValue(rt, t->branch[1], mf);
    sdef_mark(rt, &t->cancel_d, mf);
}

static STee* tee_of(JSValueConst v)
{
    return wst_opaque(v, WSTC_TEE);
}

static void tee_set_reader(JSContext* ctx, STee* t, JSValue robj, SReader* r)
{
    JSValue old = t->reader;
    t->reader = robj;
    t->r = r;
    JS_FreeValue(ctx, old);
}

static void tee_cancel_both(JSContext* ctx, STee* t, JSValueConst reason)
{
    JSValue p = wst_rs_cancel(ctx, t->s, reason);
    if (JS_IsException(p)) {
        wst_drop_exc(ctx);
        return;
    }
    sdef_resolve(ctx, &t->cancel_d, p);
    JS_FreeValue(ctx, p);
}

static JSValue tee_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv, int which)
{
    STee* t = tee_of(data);
    JSValueConst reason = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (which == 0) {
        t->canceled1 = 1;
        JS_FreeValue(ctx, t->reason1);
        t->reason1 = JS_DupValue(ctx, reason);
    } else {
        t->canceled2 = 1;
        JS_FreeValue(ctx, t->reason2);
        t->reason2 = JS_DupValue(ctx, reason);
    }
    if (t->canceled1 && t->canceled2) {
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, t->reason1));
        JS_SetPropertyUint32(ctx, arr, 1, JS_DupValue(ctx, t->reason2));
        tee_cancel_both(ctx, t, arr);
        JS_FreeValue(ctx, arr);
    }
    return JS_DupValue(ctx, t->cancel_d.p);
}

static JSValue tee_cancel1(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    return tee_cancel(ctx, data, argc, argv, 0);
}

static JSValue tee_cancel2(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    return tee_cancel(ctx, data, argc, argv, 1);
}

static JSValue tee_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv);

static JSValue tee_chunk_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    STee* t = tee_of(argv[0]);
    JSValueConst chunk = argv[1];
    JSValue pin = JS_DupValue(ctx, t->obj);
    (void)argc;
    t->read_again = 0;
    if (!t->canceled1 && wst_rsdc_enqueue(ctx, t->b[0]->c, chunk) < 0)
        wst_drop_exc(ctx);
    if (!t->canceled2 && wst_rsdc_enqueue(ctx, t->b[1]->c, chunk) < 0)
        wst_drop_exc(ctx);
    t->reading = 0;
    if (t->read_again) {
        JSValue p = tee_pull(ctx, t->obj, 0, NULL);
        JS_FreeValue(ctx, p);
    }
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue tee_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv);

static void rrt_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValueConst args[2];
    args[0] = rr->a;
    args[1] = chunk;
    JS_EnqueueJob(ctx, tee_chunk_job, 2, args);
}

static void rrt_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    STee* t = tee_of(rr->a);
    (void)chunk;
    t->reading = 0;
    if (!t->canceled1)
        wst_rsdc_close(ctx, t->b[0]->c);
    if (!t->canceled2)
        wst_rsdc_close(ctx, t->b[1]->c);
    if (!t->canceled1 || !t->canceled2)
        sdef_resolve(ctx, &t->cancel_d, JS_UNDEFINED);
}

static void rrt_error(JSContext* ctx, SReadReq* rr, JSValueConst e)
{
    (void)e;
    tee_of(rr->a)->reading = 0;
    (void)ctx;
}

static const SRROps rr_tee_ops = { rrt_chunk, rrt_close, rrt_error };

static JSValue tee_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    STee* t = tee_of(data);
    SReadReq rr;
    (void)argc;
    (void)argv;
    if (t->reading) {
        t->read_again = 1;
        return wst_resolved_undef(ctx);
    }
    t->reading = 1;
    rr.ops = &rr_tee_ops;
    rr.a = JS_DupValue(ctx, data);
    rr.b = JS_UNDEFINED;
    wst_reader_read(ctx, t->r, &rr);
    return wst_resolved_undef(ctx);
}

static JSValue tee_reader_err_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue tobj = JS_GetPropertyUint32(ctx, data, 0), robj = JS_GetPropertyUint32(ctx, data, 1);
    STee* t = tee_of(tobj);
    (void)magic;
    if (is_reject && JS_VALUE_GET_PTR(robj) == JS_VALUE_GET_PTR(t->reader)) {
        if (t->bytes) {
            wst_rbsc_error(ctx, t->b[0]->c, arg);
            wst_rbsc_error(ctx, t->b[1]->c, arg);
        } else {
            wst_rsdc_error(ctx, t->b[0]->c, arg);
            wst_rsdc_error(ctx, t->b[1]->c, arg);
        }
        if (!t->canceled1 || !t->canceled2)
            sdef_resolve(ctx, &t->cancel_d, JS_UNDEFINED);
    }
    JS_FreeValue(ctx, tobj);
    JS_FreeValue(ctx, robj);
    return JS_UNDEFINED;
}

static void tee_forward_reader_error(JSContext* ctx, STee* t)
{
    JSValue pair = JS_NewArray(ctx), rx;
    JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, t->obj));
    JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, t->reader));
    rx = JS_NewPromiseReaction(ctx, tee_reader_err_rx, pair, 0);
    JS_FreeValue(ctx, pair);
    if (!JS_IsException(rx))
        JS_PromiseReact(ctx, t->r->closed.p, rx);
    JS_FreeValue(ctx, rx);
}

static JSValue byte_pull(JSContext* ctx, STee* t, int which);

static JSValue clone_as_u8(JSContext* ctx, JSValueConst view)
{
    WVInfo vi;
    size_t n;
    uint8_t* p;
    JSValue ab, r;
    if (wst_view_info(ctx, view, &vi) <= 0)
        return JS_EXCEPTION;
    p = JS_PeekArrayBuffer(vi.buffer, &n);
    JS_FreeValue(ctx, vi.buffer);
    if (!p)
        return wst_throw(ctx, 0, "the ArrayBuffer is detached");
    ab = JS_NewArrayBufferCopy(ctx, p + (size_t)vi.off, (size_t)vi.len);
    if (JS_IsException(ab))
        return ab;
    r = wst_new_u8(ctx, ab, 0, vi.len);
    JS_FreeValue(ctx, ab);
    return r;
}

static void byte_tee_clone_fail(JSContext* ctx, STee* t, SRS* a, SRS* b)
{
    JSValue e = JS_GetException(ctx);
    wst_rbsc_error(ctx, a->c, e);
    wst_rbsc_error(ctx, b->c, e);
    tee_cancel_both(ctx, t, e);
    JS_FreeValue(ctx, e);
}

static void byte_tee_after(JSContext* ctx, STee* t)
{
    t->reading = 0;
    if (t->read_again1) {
        JSValue p = byte_pull(ctx, t, 0);
        JS_FreeValue(ctx, p);
    } else if (t->read_again2) {
        JSValue p = byte_pull(ctx, t, 1);
        JS_FreeValue(ctx, p);
    }
}

static JSValue byte_chunk_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    STee* t = tee_of(argv[0]);
    JSValueConst chunk = argv[1];
    JSValue pin = JS_DupValue(ctx, t->obj), chunk2 = JS_DupValue(ctx, chunk);
    (void)argc;
    t->read_again1 = t->read_again2 = 0;
    if (!t->canceled1 && !t->canceled2) {
        JS_FreeValue(ctx, chunk2);
        chunk2 = clone_as_u8(ctx, chunk);
        if (JS_IsException(chunk2)) {
            byte_tee_clone_fail(ctx, t, t->b[0], t->b[1]);
            JS_FreeValue(ctx, pin);
            return JS_UNDEFINED;
        }
    }
    if (!t->canceled1 && wst_rbsc_enqueue(ctx, t->b[0]->c, chunk) < 0)
        wst_drop_exc(ctx);
    if (!t->canceled2 && wst_rbsc_enqueue(ctx, t->b[1]->c, chunk2) < 0)
        wst_drop_exc(ctx);
    JS_FreeValue(ctx, chunk2);
    byte_tee_after(ctx, t);
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static void rrbd_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValueConst args[2];
    args[0] = rr->a;
    args[1] = chunk;
    JS_EnqueueJob(ctx, byte_chunk_job, 2, args);
}

static void rrbd_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    STee* t = tee_of(rr->a);
    SRBSC *c1 = t->b[0]->c, *c2 = t->b[1]->c;
    (void)chunk;
    t->reading = 0;
    if (!t->canceled1 && wst_rbsc_close(ctx, c1) < 0)
        wst_drop_exc(ctx);
    if (!t->canceled2 && wst_rbsc_close(ctx, c2) < 0)
        wst_drop_exc(ctx);
    if (c1->pending.len > 0 && wst_rbsc_respond(ctx, c1, 0) < 0)
        wst_drop_exc(ctx);
    if (c2->pending.len > 0 && wst_rbsc_respond(ctx, c2, 0) < 0)
        wst_drop_exc(ctx);
    if (!t->canceled1 || !t->canceled2)
        sdef_resolve(ctx, &t->cancel_d, JS_UNDEFINED);
}

static const SRROps rr_byte_default_ops = { rrbd_chunk, rrbd_close, rrt_error };

static JSValue byob_chunk_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    STee* t = tee_of(argv[0]);
    JSValueConst chunk = argv[1];
    int for2 = wst_bool(ctx, argv[2]);
    SRS* bb = t->b[for2];
    SRS* ob = t->b[!for2];
    int byob_canceled = for2 ? t->canceled2 : t->canceled1;
    int other_canceled = for2 ? t->canceled1 : t->canceled2;
    JSValue pin = JS_DupValue(ctx, t->obj);
    (void)argc;
    t->read_again1 = t->read_again2 = 0;
    if (!other_canceled) {
        JSValue cloned = clone_as_u8(ctx, chunk);
        if (JS_IsException(cloned)) {
            byte_tee_clone_fail(ctx, t, bb, ob);
            JS_FreeValue(ctx, pin);
            return JS_UNDEFINED;
        }
        if (!byob_canceled && wst_rbsc_respond_with_new_view(ctx, bb->c, chunk) < 0)
            wst_drop_exc(ctx);
        if (wst_rbsc_enqueue(ctx, ob->c, cloned) < 0)
            wst_drop_exc(ctx);
        JS_FreeValue(ctx, cloned);
    } else if (!byob_canceled) {
        if (wst_rbsc_respond_with_new_view(ctx, bb->c, chunk) < 0)
            wst_drop_exc(ctx);
    }
    byte_tee_after(ctx, t);
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static void rrbb_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValueConst args[3];
    args[0] = rr->a;
    args[1] = chunk;
    args[2] = rr->b;
    JS_EnqueueJob(ctx, byob_chunk_job, 3, args);
}

static void rrbb_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    STee* t = tee_of(rr->a);
    int for2 = wst_bool(ctx, rr->b);
    SRBSC* bc = t->b[for2]->c;
    SRBSC* oc = t->b[!for2]->c;
    int byob_canceled = for2 ? t->canceled2 : t->canceled1;
    int other_canceled = for2 ? t->canceled1 : t->canceled2;
    t->reading = 0;
    if (!byob_canceled && wst_rbsc_close(ctx, bc) < 0)
        wst_drop_exc(ctx);
    if (!other_canceled && wst_rbsc_close(ctx, oc) < 0)
        wst_drop_exc(ctx);
    if (!JS_IsUndefined(chunk)) {
        if (!byob_canceled && wst_rbsc_respond_with_new_view(ctx, bc, chunk) < 0)
            wst_drop_exc(ctx);
        if (!other_canceled && oc->pending.len > 0 && wst_rbsc_respond(ctx, oc, 0) < 0)
            wst_drop_exc(ctx);
    }
    if (!byob_canceled || !other_canceled)
        sdef_resolve(ctx, &t->cancel_d, JS_UNDEFINED);
}

static const SRROps rr_byte_byob_ops = { rrbb_chunk, rrbb_close, rrt_error };

static void pull_with_default_reader(JSContext* ctx, STee* t)
{
    SReadReq rr;
    if (t->r->byob) {
        SReader* nr;
        JSValue robj;
        wst_reader_release(ctx, t->r);
        robj = wst_rs_acquire_reader(ctx, t->s, 0, &nr);
        if (JS_IsException(robj)) {
            wst_drop_exc(ctx);
            return;
        }
        tee_set_reader(ctx, t, robj, nr);
        tee_forward_reader_error(ctx, t);
    }
    rr.ops = &rr_byte_default_ops;
    rr.a = JS_DupValue(ctx, t->obj);
    rr.b = JS_UNDEFINED;
    wst_reader_read(ctx, t->r, &rr);
}

static void pull_with_byob_reader(JSContext* ctx, STee* t, JSValueConst view, int for2)
{
    SReadReq rr;
    if (!t->r->byob) {
        SReader* nr;
        JSValue robj;
        wst_reader_release(ctx, t->r);
        robj = wst_rs_acquire_reader(ctx, t->s, 1, &nr);
        if (JS_IsException(robj)) {
            wst_drop_exc(ctx);
            return;
        }
        tee_set_reader(ctx, t, robj, nr);
        tee_forward_reader_error(ctx, t);
    }
    rr.ops = &rr_byte_byob_ops;
    rr.a = JS_DupValue(ctx, t->obj);
    rr.b = JS_NewBool(ctx, for2);
    wst_byob_reader_read(ctx, t->r, view, 1, &rr);
}

static JSValue byte_pull(JSContext* ctx, STee* t, int which)
{
    JSValue req;
    if (t->reading) {
        if (which == 0)
            t->read_again1 = 1;
        else
            t->read_again2 = 1;
        return wst_resolved_undef(ctx);
    }
    t->reading = 1;
    req = wst_rbsc_get_byob_request(ctx, t->b[which]->c);
    if (JS_IsException(req))
        return req;
    if (JS_IsNull(req)) {
        pull_with_default_reader(ctx, t);
    } else {
        JSValue view = wst_byobreq_view(ctx, req);
        pull_with_byob_reader(ctx, t, view, which);
        JS_FreeValue(ctx, view);
    }
    JS_FreeValue(ctx, req);
    return wst_resolved_undef(ctx);
}

static JSValue byte_pull1(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return byte_pull(ctx, tee_of(data), 0);
}

static JSValue byte_pull2(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return byte_pull(ctx, tee_of(data), 1);
}

int wst_rs_tee(JSContext* ctx, SRS* s, JSValue* out)
{
    STee* t;
    JSValue tobj, robj;
    SReader* r;
    SAlgo pull, cancel;
    int i;

    robj = wst_rs_acquire_reader(ctx, s, 0, &r);
    if (JS_IsException(robj))
        return -1;
    tobj = wst_new_obj(ctx, WSTC_TEE, JS_NULL, sizeof(STee), (void**)&t);
    if (JS_IsException(tobj)) {
        JS_FreeValue(ctx, robj);
        return -1;
    }
    t->source = JS_DupValue(ctx, s->obj);
    t->s = s;
    t->reader = robj;
    t->r = r;
    t->bytes = s->bytes;
    t->reason1 = JS_UNDEFINED;
    t->reason2 = JS_UNDEFINED;
    t->branch[0] = t->branch[1] = JS_UNDEFINED;
    sdef_init(&t->cancel_d);
    if (sdef_new(ctx, &t->cancel_d) < 0)
        goto fail;
    for (i = 0; i < 2; i++) {
        sa_init(&pull);
        sa_init(&cancel);
        if (t->bytes)
            sa_set_c(ctx, &pull, i == 0 ? byte_pull1 : byte_pull2, tobj);
        else
            sa_set_c(ctx, &pull, tee_pull, tobj);
        sa_set_c(ctx, &cancel, i == 0 ? tee_cancel1 : tee_cancel2, tobj);
        if (t->bytes)
            t->branch[i] = wst_rbs_create(ctx, JS_UNDEFINED, &pull, &cancel, &t->b[i]);
        else
            t->branch[i] = wst_rs_create(ctx, JS_UNDEFINED, &pull, &cancel, 1, JS_UNDEFINED, 0, &t->b[i]);
        if (JS_IsException(t->branch[i])) {
            t->branch[i] = JS_UNDEFINED;
            goto fail;
        }
    }
    tee_forward_reader_error(ctx, t);
    out[0] = JS_DupValue(ctx, t->branch[0]);
    out[1] = JS_DupValue(ctx, t->branch[1]);
    JS_FreeValue(ctx, tobj);
    return 0;
fail:
    JS_FreeValue(ctx, tobj);
    return -1;
}

enum { ACT_NONE, ACT_ABORT_DEST, ACT_CANCEL_SRC, ACT_CLOSE_DEST, ACT_SIGNAL };
enum { PW_SRC_ERR, PW_DST_ERR, PW_SRC_CLOSE, PW_STEP, PW_REST, PW_FINAL, PW_FINAL_ERR };

typedef struct SPipe {
    JSValue obj;
    JSValue source;
    SRS* s;
    JSValue dest;
    SWS* d;
    JSValue reader;
    SReader* r;
    JSValue writer;
    SWriter* w;
    int prevent_close, prevent_abort, prevent_cancel;
    JSValue signal;
    JSValue abort_fn;
    int shutting_down;
    JSValue current_write;
    JSValue rx_step, rx_write, cw_p;
    uint32_t w_issued, w_latest_done, cw_gen;
    SDef done;
    SDef loop_d;
    int act;
    JSValue act_arg;
    int orig_is_error;
    JSValue orig_error;
    int fin_is_error;
    JSValue fin_error;
    int dst, tj;
    JSValue dval;
} SPipe;

static void pipe_finalizer(JSRuntime* rt, JSValue val)
{
    SPipe* p = wst_opaque(val, WSTC_PIPE);
    if (!p)
        return;
    JS_FreeValueRT(rt, p->source);
    JS_FreeValueRT(rt, p->dest);
    JS_FreeValueRT(rt, p->reader);
    JS_FreeValueRT(rt, p->writer);
    JS_FreeValueRT(rt, p->signal);
    JS_FreeValueRT(rt, p->abort_fn);
    JS_FreeValueRT(rt, p->current_write);
    JS_FreeValueRT(rt, p->rx_step);
    JS_FreeValueRT(rt, p->rx_write);
    JS_FreeValueRT(rt, p->cw_p);
    sdef_free(rt, &p->done);
    sdef_free(rt, &p->loop_d);
    JS_FreeValueRT(rt, p->act_arg);
    JS_FreeValueRT(rt, p->orig_error);
    JS_FreeValueRT(rt, p->fin_error);
    JS_FreeValueRT(rt, p->dval);
    js_free_rt(rt, p);
}

static void pipe_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SPipe* p = wst_opaque(val, WSTC_PIPE);
    if (!p)
        return;
    JS_MarkValue(rt, p->source, mf);
    JS_MarkValue(rt, p->dest, mf);
    JS_MarkValue(rt, p->reader, mf);
    JS_MarkValue(rt, p->writer, mf);
    JS_MarkValue(rt, p->signal, mf);
    JS_MarkValue(rt, p->abort_fn, mf);
    JS_MarkValue(rt, p->current_write, mf);
    JS_MarkValue(rt, p->rx_step, mf);
    JS_MarkValue(rt, p->rx_write, mf);
    JS_MarkValue(rt, p->cw_p, mf);
    sdef_mark(rt, &p->done, mf);
    sdef_mark(rt, &p->loop_d, mf);
    JS_MarkValue(rt, p->act_arg, mf);
    JS_MarkValue(rt, p->orig_error, mf);
    JS_MarkValue(rt, p->fin_error, mf);
    JS_MarkValue(rt, p->dval, mf);
}

static SPipe* pipe_of(JSValueConst v)
{
    return wst_opaque(v, WSTC_PIPE);
}

static int ws_close_queued_or_in_flight(SWS* d)
{
    return wst_ws_close_queued_or_in_flight(d);
}

static JSValue pipe_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg);

static int pipe_react(JSContext* ctx, SPipe* p, JSValueConst promise, int kind)
{
    JSValue rx;
    int r = -1;
    if (JS_IsException(promise))
        return -1;
    rx = JS_NewPromiseReaction(ctx, pipe_rx, p->obj, kind);
    if (!JS_IsException(rx))
        r = JS_PromiseReact(ctx, promise, rx);
    JS_FreeValue(ctx, rx);
    return r;
}

static void pipe_finalize(JSContext* ctx, SPipe* p, int is_error, JSValueConst error)
{
    JSValue pin = JS_DupValue(ctx, p->obj);
    WstRt* w = wst_rt(ctx);
    if (!w)
        return;
    if (p->w && p->w->s)
        wst_writer_release(ctx, p->w);
    if (p->r && p->r->s)
        wst_reader_release(ctx, p->r);
    if (!JS_IsUndefined(p->signal)) {
        JSValue args[2], r;
        args[0] = JS_NewString(ctx, "abort");
        args[1] = p->abort_fn;
        r = JS_Call(ctx, w->sig_remove, p->signal, 2, (JSValueConst*)args);
        if (JS_IsException(r))
            wst_drop_exc(ctx);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
    }
    if (is_error)
        sdef_reject(ctx, &p->done, error);
    else
        sdef_resolve(ctx, &p->done, JS_UNDEFINED);
    JS_FreeValue(ctx, pin);
}

static JSValue pipe_wfw_f(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg);

static JSValue pipe_current_write(JSContext* ctx, SPipe* p)
{
    if (p->w_latest_done == p->w_issued)
        return JS_DupValue(ctx, p->current_write);
    if (JS_IsUndefined(p->cw_p) || p->cw_gen != p->w_issued) {
        JS_FreeValue(ctx, p->cw_p);
        p->cw_p = JS_NewPromisePending(ctx);
        if (JS_IsException(p->cw_p)) {
            p->cw_p = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
        p->cw_gen = p->w_issued;
    }
    return JS_DupValue(ctx, p->cw_p);
}

static JSValue pipe_wait_for_writes(JSContext* ctx, SPipe* p)
{
    JSValue cw = pipe_current_write(ctx, p), q;
    if (JS_IsException(cw))
        return cw;
    q = wst_then(ctx, cw, pipe_wfw_f, p->obj, (int)p->w_issued);
    JS_FreeValue(ctx, cw);
    return q;
}

static JSValue pipe_wfw_f(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SPipe* p = pipe_of(data);
    if (is_reject)
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    if ((uint32_t)magic != p->w_issued)
        return pipe_wait_for_writes(ctx, p);
    return JS_UNDEFINED;
}

static JSValue wait_all_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue st = JS_GetPropertyUint32(ctx, data, 0), dp = JS_GetPropertyUint32(ctx, data, 1);
    int32_t left, failed;
    int rc = 0;
    JSValue lv = JS_GetPropertyUint32(ctx, st, 0), fv = JS_GetPropertyUint32(ctx, st, 1);
    (void)magic;
    left = JS_IsNumber(lv) ? (int32_t)dyn_num_value(lv) : 0;
    failed = JS_ToBool(ctx, fv);
    if (!is_reject) {
        left--;
        JS_SetPropertyUint32(ctx, st, 0, JS_NewInt32(ctx, left));
        if (left == 0 && !failed)
            rc = dyn_settle(ctx, dp, JS_UNDEFINED, 0);
    } else if (!failed) {
        JS_SetPropertyUint32(ctx, st, 1, JS_TRUE);
        rc = dyn_settle(ctx, dp, arg, 1);
    }
    JS_FreeValue(ctx, lv);
    JS_FreeValue(ctx, fv);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, dp);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue wait_all(JSContext* ctx, JSValue* ps, int n)
{
    JSValue d, st, pair, rx;
    int i;
    if (n == 0)
        return wst_resolved_undef(ctx);
    d = JS_NewPromisePending(ctx);
    st = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, st, 0, JS_NewInt32(ctx, n));
    JS_SetPropertyUint32(ctx, st, 1, JS_FALSE);
    pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, st);
    JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, d));
    for (i = 0; i < n; i++) {
        rx = JS_NewPromiseReaction(ctx, wait_all_rx, pair, 0);
        if (!JS_IsException(rx))
            JS_PromiseReact(ctx, ps[i], rx);
        JS_FreeValue(ctx, rx);
    }
    JS_FreeValue(ctx, pair);
    return d;
}

static JSValue pipe_run_action(JSContext* ctx, SPipe* p)
{
    JSValue r;
    switch (p->act) {
    case ACT_ABORT_DEST:
        r = wst_ws_abort(ctx, p->d, p->act_arg);
        break;
    case ACT_CANCEL_SRC:
        r = wst_rs_cancel(ctx, p->s, p->act_arg);
        break;
    case ACT_CLOSE_DEST:
        r = wst_writer_close_with_error_propagation(ctx, p->w);
        break;
    default: {
        JSValue ps[2];
        int n = 0, i;
        if (!p->prevent_abort)
            ps[n++] = p->d->state == WS_WRITABLE ? wst_ws_abort(ctx, p->d, p->act_arg) : wst_resolved_undef(ctx);
        if (!p->prevent_cancel)
            ps[n++] = p->s->state == RS_READABLE ? wst_rs_cancel(ctx, p->s, p->act_arg) : wst_resolved_undef(ctx);
        for (i = 0; i < n; i++)
            if (JS_IsException(ps[i]))
                ps[i] = wst_reject_exc(ctx);
        if (JS_HasUncatchableException(ctx)) {
            for (i = 0; i < n; i++)
                JS_FreeValue(ctx, ps[i]);
            return JS_EXCEPTION;
        }
        r = wait_all(ctx, ps, n);
        for (i = 0; i < n; i++)
            JS_FreeValue(ctx, ps[i]);
        break;
    }
    }
    if (JS_IsException(r))
        r = wst_reject_exc(ctx);
    return r;
}

static void pipe_do_the_rest(JSContext* ctx, SPipe* p)
{
    JSValue r = pipe_run_action(ctx, p);
    pipe_react(ctx, p, r, PW_REST);
    JS_FreeValue(ctx, r);
}

static void pipe_shutdown_with_action(JSContext* ctx, SPipe* p, int act, JSValueConst arg, int orig_is_error, JSValueConst orig_error)
{
    if (p->shutting_down)
        return;
    p->shutting_down = 1;
    p->act = act;
    p->act_arg = JS_DupValue(ctx, arg);
    p->orig_is_error = orig_is_error;
    p->orig_error = JS_DupValue(ctx, orig_error);
    if (p->d->state == WS_WRITABLE && !ws_close_queued_or_in_flight(p->d)) {
        JSValue q = pipe_wait_for_writes(ctx, p);
        if (!JS_IsException(q))
            pipe_react(ctx, p, q, PW_STEP + 100);
        JS_FreeValue(ctx, q);
    } else {
        pipe_do_the_rest(ctx, p);
    }
}

static void pipe_shutdown(JSContext* ctx, SPipe* p, int is_error, JSValueConst error)
{
    if (p->shutting_down)
        return;
    p->shutting_down = 1;
    if (p->d->state == WS_WRITABLE && !ws_close_queued_or_in_flight(p->d)) {
        JSValue q;
        p->fin_is_error = is_error;
        p->fin_error = JS_DupValue(ctx, error);
        q = pipe_wait_for_writes(ctx, p);
        if (!JS_IsException(q))
            pipe_react(ctx, p, q, PW_FINAL);
        JS_FreeValue(ctx, q);
    } else {
        pipe_finalize(ctx, p, is_error, error);
    }
}

static void pipe_on_src_error(JSContext* ctx, SPipe* p, JSValueConst e)
{
    if (!p->prevent_abort)
        pipe_shutdown_with_action(ctx, p, ACT_ABORT_DEST, e, 1, e);
    else
        pipe_shutdown(ctx, p, 1, e);
}

static void pipe_on_dst_error(JSContext* ctx, SPipe* p, JSValueConst e)
{
    if (!p->prevent_cancel)
        pipe_shutdown_with_action(ctx, p, ACT_CANCEL_SRC, e, 1, e);
    else
        pipe_shutdown(ctx, p, 1, e);
}

static void pipe_on_src_close(JSContext* ctx, SPipe* p)
{
    if (!p->prevent_close)
        pipe_shutdown_with_action(ctx, p, ACT_CLOSE_DEST, JS_UNDEFINED, 0, JS_UNDEFINED);
    else
        pipe_shutdown(ctx, p, 0, JS_UNDEFINED);
}

static JSValue pipe_write_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SPipe* p = pipe_of(data);
    int64_t tag;
    (void)magic;
    (void)is_reject;
    tag = JS_IsNumber(arg) ? (int64_t)dyn_num_value(arg) : -1;
    if ((uint32_t)tag == p->w_issued)
        p->w_latest_done = p->w_issued;
    if (!JS_IsUndefined(p->cw_p) && p->cw_gen == (uint32_t)tag) {
        JSValue q = p->cw_p;
        int rc;
        p->cw_p = JS_UNDEFINED;
        rc = dyn_settle(ctx, q, JS_UNDEFINED, 0);
        JS_FreeValue(ctx, q);
        if (rc < 0)
            return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static void pipe_step_settled(JSContext* ctx, SPipe* p, JSValueConst v, int is_reject);

static JSValue pipe_q_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)argc;
    pipe_step_settled(ctx, pipe_of(argv[0]), argv[1], wst_bool(ctx, argv[2]));
    return JS_UNDEFINED;
}

static void pipe_q_settle(JSContext* ctx, SPipe* p, JSValueConst v, int is_reject)
{
    JSValueConst args[3];
    args[0] = p->obj;
    args[1] = v;
    args[2] = JS_NewBool(ctx, is_reject);
    JS_EnqueueJob(ctx, pipe_q_job, 3, args);
}

static JSValue pipe_d_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)argc;
    pipe_q_settle(ctx, pipe_of(argv[0]), argv[1], wst_bool(ctx, argv[2]));
    return JS_UNDEFINED;
}

static void pipe_d_react(JSContext* ctx, SPipe* p)
{
    JSValueConst args[3];
    args[0] = p->obj;
    args[1] = p->dval;
    args[2] = JS_NewBool(ctx, p->dst == 2);
    JS_EnqueueJob(ctx, pipe_d_job, 3, args);
}

static void pipe_d_settle(JSContext* ctx, SPipe* p, JSValueConst v, int is_reject)
{
    if (p->dst)
        return;
    p->dst = is_reject ? 2 : 1;
    JS_FreeValue(ctx, p->dval);
    p->dval = JS_DupValue(ctx, v);
    if (p->tj)
        pipe_d_react(ctx, p);
}

static JSValue pipe_tj_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    SPipe* p = pipe_of(argv[0]);
    (void)argc;
    p->tj = 1;
    if (p->dst)
        pipe_d_react(ctx, p);
    return JS_UNDEFINED;
}

static JSValue pipe_chunk_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    SPipe* p = pipe_of(argv[0]);
    JSValueConst chunk = argv[1];
    (void)argc;
    if (!p->w || !p->w->s) {
        pipe_d_settle(ctx, p, JS_FALSE, 0);
        return JS_UNDEFINED;
    }
    if (JS_IsException(wst_rx(ctx, &p->rx_write, pipe_write_done, p->obj, 0))) {
        p->rx_write = JS_UNDEFINED;
        wst_drop_exc(ctx);
    } else {
        wst_writer_write_native(ctx, p->w, chunk, p->rx_write, ++p->w_issued);
    }
    pipe_d_settle(ctx, p, JS_FALSE, 0);
    return JS_UNDEFINED;
}

static void rrp_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    JSValueConst args[2];
    args[0] = rr->b;
    args[1] = chunk;
    JS_EnqueueJob(ctx, pipe_chunk_job, 2, args);
}

static void rrp_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk)
{
    (void)chunk;
    pipe_d_settle(ctx, pipe_of(rr->b), JS_TRUE, 0);
}

static void rrp_error(JSContext* ctx, SReadReq* rr, JSValueConst e)
{
    pipe_d_settle(ctx, pipe_of(rr->b), e, 1);
}

static const SRROps rr_pipe_ops = { rrp_chunk, rrp_close, rrp_error };

static JSValue pipe_step_f(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SPipe* p = pipe_of(data);
    SReadReq rr;
    (void)magic;
    if (is_reject) {
        pipe_q_settle(ctx, p, arg, 1);
        return JS_UNDEFINED;
    }
    p->dst = 0;
    p->tj = 0;
    JS_FreeValue(ctx, p->dval);
    p->dval = JS_UNDEFINED;
    rr.ops = &rr_pipe_ops;
    rr.a = JS_UNDEFINED;
    rr.b = JS_DupValue(ctx, p->obj);
    wst_reader_read(ctx, p->r, &rr);
    JS_EnqueueJob(ctx, pipe_tj_job, 1, (JSValueConst*)&p->obj);
    return JS_UNDEFINED;
}

static void pipe_next(JSContext* ctx, SPipe* p, int is_done)
{
    JSValue rx;
    if (is_done) {
        sdef_resolve(ctx, &p->loop_d, JS_UNDEFINED);
        return;
    }
    if (p->shutting_down) {
        pipe_q_settle(ctx, p, JS_TRUE, 0);
        return;
    }
    rx = wst_rx(ctx, &p->rx_step, pipe_step_f, p->obj, 0);
    if (JS_IsException(rx)) {
        p->rx_step = JS_UNDEFINED;
        wst_drop_exc(ctx);
        return;
    }
    JS_PromiseReact(ctx, p->w->ready.p, rx);
}

static void pipe_step_settled(JSContext* ctx, SPipe* p, JSValueConst v, int is_reject)
{
    JSValue pin = JS_DupValue(ctx, p->obj);
    if (is_reject)
        sdef_reject(ctx, &p->loop_d, v);
    else
        pipe_next(ctx, p, wst_bool(ctx, v));
    JS_FreeValue(ctx, pin);
}

static JSValue pipe_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SPipe* p = pipe_of(data);
    JSValue pin = JS_DupValue(ctx, p->obj);
    switch (magic) {
    case PW_SRC_ERR:
        if (is_reject)
            pipe_on_src_error(ctx, p, arg);
        break;
    case PW_DST_ERR:
        if (is_reject)
            pipe_on_dst_error(ctx, p, arg);
        break;
    case PW_SRC_CLOSE:
        if (!is_reject)
            pipe_on_src_close(ctx, p);
        break;
    case PW_STEP + 100:
        if (!is_reject)
            pipe_do_the_rest(ctx, p);
        break;
    case PW_REST:
        if (is_reject)
            pipe_finalize(ctx, p, 1, arg);
        else
            pipe_finalize(ctx, p, p->orig_is_error, p->orig_error);
        break;
    case PW_FINAL:
        if (!is_reject)
            pipe_finalize(ctx, p, p->fin_is_error, p->fin_error);
        break;
    }
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

static JSValue pipe_abort_algorithm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* func_data)
{
    SPipe* p = pipe_of(func_data[0]);
    JSValue error, pin;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    pin = JS_DupValue(ctx, p->obj);
    error = JS_GetPropertyStr(ctx, p->signal, "reason");
    if (JS_IsException(error)) {
        JS_FreeValue(ctx, pin);
        return error;
    }
    pipe_shutdown_with_action(ctx, p, ACT_SIGNAL, error, 1, error);
    JS_FreeValue(ctx, error);
    JS_FreeValue(ctx, pin);
    return JS_UNDEFINED;
}

JSValue wst_rs_pipe_to(JSContext* ctx, SRS* s, SWS* d, int prevent_close, int prevent_abort, int prevent_cancel, JSValueConst signal)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    SPipe* p;
    JSValue pobj, ret, loop;

    if (!JS_IsUndefined(s->native) && !JS_IsUndefined(d->native) && JS_IsUndefined(signal)) {
        JSValue fp = wst_fast_pipe(ctx, s, d, prevent_close, prevent_abort, prevent_cancel);
        if (!JS_IsUndefined(fp))
            return fp;
    }
    pobj = wst_new_obj(ctx, WSTC_PIPE, JS_NULL, sizeof(SPipe), (void**)&p);
    if (JS_IsException(pobj))
        return pobj;
    p->source = JS_DupValue(ctx, s->obj);
    p->s = s;
    p->dest = JS_DupValue(ctx, d->obj);
    p->d = d;
    p->reader = JS_UNDEFINED;
    p->writer = JS_UNDEFINED;
    p->signal = JS_DupValue(ctx, signal);
    p->abort_fn = JS_UNDEFINED;
    p->current_write = JS_UNDEFINED;
    p->rx_step = JS_UNDEFINED;
    p->rx_write = JS_UNDEFINED;
    p->cw_p = JS_UNDEFINED;
    sdef_init(&p->done);
    sdef_init(&p->loop_d);
    p->act_arg = JS_UNDEFINED;
    p->orig_error = JS_UNDEFINED;
    p->fin_error = JS_UNDEFINED;
    p->dval = JS_UNDEFINED;
    p->prevent_close = prevent_close;
    p->prevent_abort = prevent_abort;
    p->prevent_cancel = prevent_cancel;
    p->reader = wst_rs_acquire_reader(ctx, s, 0, &p->r);
    if (JS_IsException(p->reader)) {
        p->reader = JS_UNDEFINED;
        goto fail;
    }
    p->writer = wst_ws_acquire_writer(ctx, d, &p->w);
    if (JS_IsException(p->writer)) {
        p->writer = JS_UNDEFINED;
        goto fail;
    }
    s->disturbed = 1;
    p->current_write = wst_resolved_undef(ctx);
    if (sdef_new(ctx, &p->done) < 0)
        goto fail;
    if (!JS_IsUndefined(signal)) {
        JSValue aborted, args[2], r;
        int ab;
        p->abort_fn = JS_NewCFunctionData(ctx, pipe_abort_algorithm, 0, 0, 1, (JSValueConst*)&pobj);
        if (JS_IsException(p->abort_fn)) {
            p->abort_fn = JS_UNDEFINED;
            goto fail;
        }
        aborted = JS_GetPropertyStr(ctx, signal, "aborted");
        if (JS_IsException(aborted))
            goto fail;
        ab = JS_ToBool(ctx, aborted);
        JS_FreeValue(ctx, aborted);
        if (ab) {
            r = JS_Call(ctx, p->abort_fn, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r))
                goto fail;
            JS_FreeValue(ctx, r);
            ret = JS_DupValue(ctx, p->done.p);
            JS_FreeValue(ctx, pobj);
            return ret;
        }
        args[0] = JS_NewString(ctx, "abort");
        args[1] = p->abort_fn;
        r = JS_Call(ctx, w->sig_add, signal, 2, (JSValueConst*)args);
        JS_FreeValue(ctx, args[0]);
        if (JS_IsException(r))
            goto fail;
        JS_FreeValue(ctx, r);
    }
    if (s->state == RS_ERRORED)
        pipe_on_src_error(ctx, p, s->stored_error);
    else
        pipe_react(ctx, p, p->r->closed.p, PW_SRC_ERR);
    if (d->state == WS_ERRORED)
        pipe_on_dst_error(ctx, p, d->stored_error);
    else
        pipe_react(ctx, p, p->w->closed.p, PW_DST_ERR);
    if (s->state == RS_CLOSED)
        pipe_on_src_close(ctx, p);
    else
        pipe_react(ctx, p, p->r->closed.p, PW_SRC_CLOSE);
    if (ws_close_queued_or_in_flight(d) || d->state == WS_CLOSED) {
        JSValue e = wst_err(ctx, 0, "the destination writable stream closed before all data could be piped to it");
        pipe_on_dst_error(ctx, p, e);
        JS_FreeValue(ctx, e);
    }
    if (sdef_new(ctx, &p->loop_d) < 0)
        goto fail;
    loop = JS_DupValue(ctx, p->loop_d.p);
    pipe_next(ctx, p, 0);
    JS_PromiseMarkHandled(ctx, loop);
    JS_FreeValue(ctx, loop);
    ret = JS_DupValue(ctx, p->done.p);
    JS_FreeValue(ctx, pobj);
    return ret;
fail:
    JS_FreeValue(ctx, pobj);
    return JS_EXCEPTION;
}

static int pipe_options(JSContext* ctx, JSValueConst o, int* pc, int* pa, int* pcan, JSValue* signal)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return -1;
    JSValue d, v;
    *signal = JS_UNDEFINED;
    if (wst_to_dict(ctx, o, "options", &d) < 0)
        return -1;
    v = wst_dict_get(ctx, d, WSA_preventAbort);
    if (JS_IsException(v))
        goto fail;
    *pa = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = wst_dict_get(ctx, d, WSA_preventCancel);
    if (JS_IsException(v))
        goto fail;
    *pcan = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = wst_dict_get(ctx, d, WSA_preventClose);
    if (JS_IsException(v))
        goto fail;
    *pc = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = wst_dict_get(ctx, d, WSA_signal);
    if (JS_IsException(v))
        goto fail;
    if (!JS_IsUndefined(v)) {
        int ok = JS_IsFunction(ctx, w->sig_ctor) ? JS_IsInstanceOf(ctx, v, w->sig_ctor) : 0;
        if (ok < 0) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
        if (!ok) {
            JS_FreeValue(ctx, v);
            wst_throw(ctx, 0, "options.signal must be an AbortSignal");
            goto fail;
        }
    }
    *signal = v;
    JS_FreeValue(ctx, d);
    return 0;
fail:
    JS_FreeValue(ctx, d);
    return -1;
}

JSValue wst_rs_pipe_to_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_opaque(this_val, WSTC_RS);
    SWS* d;
    JSValue signal, r;
    int pc, pa, pcan;
    if (!s) {
        wst_throw(ctx, 0, "this is not a ReadableStream");
        return wst_reject_exc(ctx);
    }
    d = wst_opaque(argc > 0 ? argv[0] : JS_UNDEFINED, WSTC_WS);
    if (!d) {
        wst_throw(ctx, 0, "pipeTo: destination is not a WritableStream");
        return wst_reject_exc(ctx);
    }
    if (pipe_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &pc, &pa, &pcan, &signal) < 0)
        return wst_reject_exc(ctx);
    if (wst_rs_locked(s)) {
        JS_FreeValue(ctx, signal);
        return wst_reject_err(ctx, 0, "pipeTo: the source ReadableStream is locked");
    }
    if (wst_ws_locked(d)) {
        JS_FreeValue(ctx, signal);
        return wst_reject_err(ctx, 0, "pipeTo: the destination WritableStream is locked");
    }
    r = wst_rs_pipe_to(ctx, s, d, pc, pa, pcan, signal);
    JS_FreeValue(ctx, signal);
    if (JS_IsException(r))
        return wst_reject_exc(ctx);
    return r;
}

JSValue wst_rs_pipe_through_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRS* s = wst_rs_of(ctx, this_val, "this");
    JSValue pair, readable, writable, signal, r;
    SWS* d;
    int pc, pa, pcan;
    if (!s)
        return JS_EXCEPTION;
    if (wst_to_dict(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "transform", &pair) < 0)
        return JS_EXCEPTION;
    if (JS_IsNull(pair))
        return wst_throw(ctx, 0, "pipeThrough: transform must be an object");
    readable = JS_GetProperty(ctx, pair, wst_atom(ctx, WSA_readable));
    if (JS_IsException(readable)) {
        JS_FreeValue(ctx, pair);
        return readable;
    }
    if (!wst_opaque(readable, WSTC_RS)) {
        JS_FreeValue(ctx, pair);
        JS_FreeValue(ctx, readable);
        return wst_throw(ctx, 0, "pipeThrough: transform.readable is not a ReadableStream");
    }
    writable = JS_GetProperty(ctx, pair, wst_atom(ctx, WSA_writable));
    JS_FreeValue(ctx, pair);
    if (JS_IsException(writable)) {
        JS_FreeValue(ctx, readable);
        return writable;
    }
    d = wst_opaque(writable, WSTC_WS);
    if (!d) {
        JS_FreeValue(ctx, readable);
        JS_FreeValue(ctx, writable);
        return wst_throw(ctx, 0, "pipeThrough: transform.writable is not a WritableStream");
    }
    if (pipe_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &pc, &pa, &pcan, &signal) < 0)
        goto fail;
    if (wst_rs_locked(s)) {
        JS_FreeValue(ctx, signal);
        wst_throw(ctx, 0, "pipeThrough: the source ReadableStream is locked");
        goto fail;
    }
    if (wst_ws_locked(d)) {
        JS_FreeValue(ctx, signal);
        wst_throw(ctx, 0, "pipeThrough: transform.writable is locked");
        goto fail;
    }
    r = wst_rs_pipe_to(ctx, s, d, pc, pa, pcan, signal);
    JS_FreeValue(ctx, signal);
    if (JS_IsException(r))
        goto fail;
    JS_PromiseMarkHandled(ctx, r);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, writable);
    return readable;
fail:
    JS_FreeValue(ctx, readable);
    JS_FreeValue(ctx, writable);
    return JS_EXCEPTION;
}

typedef struct SFrom {
    JSValue obj;
    JSValue it;
    JSValue next;
    int sync;
    JSValue stream;
    SRS* s;
} SFrom;

static void from_finalizer(JSRuntime* rt, JSValue val)
{
    SFrom* f = wst_opaque(val, WSTC_FROM);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->it);
    JS_FreeValueRT(rt, f->next);
    JS_FreeValueRT(rt, f->stream);
    js_free_rt(rt, f);
}

static void from_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SFrom* f = wst_opaque(val, WSTC_FROM);
    if (!f)
        return;
    JS_MarkValue(rt, f->it, mf);
    JS_MarkValue(rt, f->next, mf);
    JS_MarkValue(rt, f->stream, mf);
}

static void close_sync(JSContext* ctx, JSValueConst it)
{
    JSValue r = JS_GetPropertyStr(ctx, it, "return");
    if (JS_IsException(r)) {
        wst_drop_exc(ctx);
        return;
    }
    if (!JS_IsUndefined(r) && !JS_IsNull(r)) {
        JSValue x = JS_Call(ctx, r, it, 0, NULL);
        if (JS_IsException(x))
            wst_drop_exc(ctx);
        JS_FreeValue(ctx, x);
    }
    JS_FreeValue(ctx, r);
}

static JSValue sync_value_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    if (is_reject) {
        if (!magic) {
            JSValue it = JS_GetPropertyUint32(ctx, data, 0);
            close_sync(ctx, it);
            JS_FreeValue(ctx, it);
        }
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    }
    return wst_iter_result(ctx, arg, magic & 1);
}

static JSValue sync_step(JSContext* ctx, JSValueConst it, JSValueConst res, int with_close)
{
    JSValue dv, value, p, q, holder;
    int done;
    if (!JS_IsObject(res))
        return wst_throw(ctx, 0, "iterator result is not an object");
    dv = JS_GetProperty(ctx, res, wst_atom(ctx, WSA_done));
    if (JS_IsException(dv))
        return dv;
    done = JS_ToBool(ctx, dv);
    JS_FreeValue(ctx, dv);
    value = JS_GetProperty(ctx, res, wst_atom(ctx, WSA_value));
    if (JS_IsException(value))
        return value;
    p = JS_PromiseResolveValue(ctx, value);
    JS_FreeValue(ctx, value);
    if (JS_IsException(p))
        return p;
    holder = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, holder, 0, JS_DupValue(ctx, it));
    q = wst_then(ctx, p, sync_value_rx, holder, done | (with_close ? 0 : 2));
    JS_FreeValue(ctx, holder);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue from_iter_next(JSContext* ctx, SFrom* f)
{
    JSValue res;
    if (!JS_IsFunction(ctx, f->next))
        return wst_throw(ctx, 0, "iterator.next is not a function");
    res = JS_Call(ctx, f->next, f->it, 0, NULL);
    if (!f->sync || JS_IsException(res))
        return res;
    {
        JSValue q = sync_step(ctx, f->it, res, 1);
        JS_FreeValue(ctx, res);
        return q;
    }
}

static JSValue from_pull_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SFrom* f = wst_opaque(data, WSTC_FROM);
    JSValue dv, v;
    (void)magic;
    if (is_reject)
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    if (!JS_IsObject(arg))
        return wst_throw(ctx, 0, "iterator result is not an object");
    dv = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_done));
    if (JS_IsException(dv))
        return dv;
    if (JS_ToBool(ctx, dv)) {
        JS_FreeValue(ctx, dv);
        wst_rsdc_close(ctx, f->s->c);
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, dv);
    v = JS_GetProperty(ctx, arg, wst_atom(ctx, WSA_value));
    if (JS_IsException(v))
        return v;
    if (wst_rsdc_enqueue(ctx, f->s->c, v) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    return JS_UNDEFINED;
}

static JSValue from_pull(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SFrom* f = wst_opaque(data, WSTC_FROM);
    JSValue nr, p, q;
    (void)argc;
    (void)argv;
    nr = from_iter_next(ctx, f);
    if (JS_IsException(nr))
        return wst_reject_exc(ctx);
    p = JS_PromiseResolveValue(ctx, nr);
    JS_FreeValue(ctx, nr);
    if (JS_IsException(p))
        return wst_reject_exc(ctx);
    q = wst_then(ctx, p, from_pull_rx, data, 0);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue from_cancel_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)data;
    (void)magic;
    if (is_reject)
        return JS_Throw(ctx, JS_DupValue(ctx, arg));
    if (!JS_IsObject(arg))
        return wst_throw(ctx, 0, "iterator.return() result is not an object");
    return JS_UNDEFINED;
}

static JSValue sync_return(JSContext* ctx, JSValueConst it, JSValueConst v)
{
    JSValue r = JS_GetPropertyStr(ctx, it, "return"), res, q;
    if (JS_IsException(r))
        return r;
    if (JS_IsUndefined(r) || JS_IsNull(r)) {
        JSValue o = wst_iter_result(ctx, v, 1);
        q = JS_PromiseResolveValue(ctx, o);
        JS_FreeValue(ctx, o);
        return q;
    }
    res = JS_Call(ctx, r, it, 1, &v);
    JS_FreeValue(ctx, r);
    if (JS_IsException(res))
        return res;
    q = sync_step(ctx, it, res, 0);
    JS_FreeValue(ctx, res);
    return q;
}

static JSValue from_cancel(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv)
{
    SFrom* f = wst_opaque(data, WSTC_FROM);
    JSValueConst reason = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue ret, rr, p, q;
    if (f->sync) {
        rr = sync_return(ctx, f->it, reason);
    } else {
        ret = JS_GetPropertyStr(ctx, f->it, "return");
        if (JS_IsException(ret))
            return wst_reject_exc(ctx);
        if (JS_IsUndefined(ret) || JS_IsNull(ret))
            return wst_resolved_undef(ctx);
        if (!JS_IsFunction(ctx, ret)) {
            JS_FreeValue(ctx, ret);
            wst_throw(ctx, 0, "iterator.return is not a function");
            return wst_reject_exc(ctx);
        }
        rr = JS_Call(ctx, ret, f->it, 1, &reason);
        JS_FreeValue(ctx, ret);
    }
    if (JS_IsException(rr))
        return wst_reject_exc(ctx);
    p = JS_PromiseResolveValue(ctx, rr);
    JS_FreeValue(ctx, rr);
    if (JS_IsException(p))
        return wst_reject_exc(ctx);
    q = wst_then(ctx, p, from_cancel_rx, JS_UNDEFINED, 0);
    JS_FreeValue(ctx, p);
    return q;
}

static JSValue get_sym(JSContext* ctx, JSValueConst obj, const char* name)
{
    JSValue g = JS_GetGlobalObject(ctx), sym = JS_GetPropertyStr(ctx, g, "Symbol"), k = JS_GetPropertyStr(ctx, sym, name), v;
    JSAtom a = JS_ValueToAtom(ctx, k);
    v = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
    return v;
}

JSValue wst_rs_from_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst obj = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue method = JS_UNDEFINED, it, next, fobj, sobj;
    int sync = 0;
    SFrom* f;
    SAlgo pull, cancel;
    (void)this_val;
    if (!JS_IsObject(obj))
        return wst_throw(ctx, 0, "ReadableStream.from: the argument must be an iterable object");
    if (!JS_IsUndefined(obj) && !JS_IsNull(obj)) {
        method = get_sym(ctx, obj, "asyncIterator");
        if (JS_IsException(method))
            return method;
    }
    if (JS_IsUndefined(method) || JS_IsNull(method)) {
        JSValue sm = JS_UNDEFINED;
        if (!JS_IsUndefined(obj) && !JS_IsNull(obj)) {
            sm = get_sym(ctx, obj, "iterator");
            if (JS_IsException(sm))
                return sm;
        }
        if (JS_IsUndefined(sm) || JS_IsNull(sm))
            return wst_throw(ctx, 0, "ReadableStream.from: the argument is not async iterable or iterable");
        if (!JS_IsFunction(ctx, sm)) {
            JS_FreeValue(ctx, sm);
            return wst_throw(ctx, 0, "ReadableStream.from: @@iterator is not a function");
        }
        it = JS_Call(ctx, sm, obj, 0, NULL);
        JS_FreeValue(ctx, sm);
        if (JS_IsException(it))
            return it;
        if (!JS_IsObject(it)) {
            JS_FreeValue(ctx, it);
            return wst_throw(ctx, 0, "ReadableStream.from: @@iterator did not return an object");
        }
        sync = 1;
    } else {
        if (!JS_IsFunction(ctx, method)) {
            JS_FreeValue(ctx, method);
            return wst_throw(ctx, 0, "ReadableStream.from: @@asyncIterator is not a function");
        }
        it = JS_Call(ctx, method, obj, 0, NULL);
        JS_FreeValue(ctx, method);
        if (JS_IsException(it))
            return it;
        if (!JS_IsObject(it)) {
            JS_FreeValue(ctx, it);
            return wst_throw(ctx, 0, "ReadableStream.from: @@asyncIterator did not return an object");
        }
    }
    next = JS_GetPropertyStr(ctx, it, "next");
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, it);
        return next;
    }
    fobj = wst_new_obj(ctx, WSTC_FROM, JS_NULL, sizeof(SFrom), (void**)&f);
    if (JS_IsException(fobj)) {
        JS_FreeValue(ctx, it);
        JS_FreeValue(ctx, next);
        return fobj;
    }
    f->it = it;
    f->next = next;
    f->sync = sync;
    f->stream = JS_UNDEFINED;
    sa_init(&pull);
    sa_init(&cancel);
    sa_set_c(ctx, &pull, from_pull, fobj);
    sa_set_c(ctx, &cancel, from_cancel, fobj);
    sobj = wst_rs_create(ctx, JS_UNDEFINED, &pull, &cancel, 0, JS_UNDEFINED, 0, &f->s);
    if (!JS_IsException(sobj))
        f->stream = JS_DupValue(ctx, sobj);
    JS_FreeValue(ctx, fobj);
    return sobj;
}

int wst_init_pipe(JSContext* ctx)
{
    static const WstClassSpec specs[] = {
        { WSTC_TEE, "WstTee", NULL, 0, NULL, 0, NULL, 0, tee_finalizer, tee_mark },
        { WSTC_PIPE, "WstPipe", NULL, 0, NULL, 0, NULL, 0, pipe_finalizer, pipe_mark },
        { WSTC_FROM, "WstFrom", NULL, 0, NULL, 0, NULL, 0, from_finalizer, from_mark },
    };
    JSValue tmp;
    size_t i;
    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &tmp) < 0)
            return -1;
    return 0;
}
