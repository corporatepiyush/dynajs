#include "stream-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

void dyn_cps_anchor_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_cps_arm_t* a = JS_GetOpaque(val, dyn_cps_anchor_class_id);
    (void)rt;
    if (!a)
        return;
    if (a->cp && a->cp->arm == a)
        a->cp->arm = NULL;
    JS_SetOpaque(val, NULL);
    free(a);
}

void dyn_cps_arm_kill(JSContext* ctx, JSRuntime* rt, dyn_cps_t* cp)
{
    dyn_cps_arm_t* a = cp->arm;
    (void)ctx;
    if (!a)
        return;
    cp->arm = NULL;
    a->cp = NULL;
    JS_SetOpaque(a->anchor, NULL);
    a->anchor = JS_UNDEFINED;
    free(a);
}

static JSValue stream_swallow_rejection(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue dyn_cps_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValueConst* func_data)
{
    dyn_cps_arm_t* a;
    dyn_cps_t* cp;

    (void)this_val;
    (void)argc;
    a = JS_GetOpaque(func_data[0], dyn_cps_anchor_class_id);
    if (!a || a->settled || !a->cp)
        return JS_UNDEFINED;
    a->settled = 1;
    cp = a->cp;
    if (cp->dead)
        return JS_UNDEFINED;
    cp->step(ctx, cp, argc > 0 ? argv[0] : JS_UNDEFINED, magic);
    return stream_ret(ctx, JS_UNDEFINED);
}

static JSValue dyn_cps_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    dyn_cps_t* cp;
    int64_t p = 0;

    if (JS_ToInt64(ctx, &p, argv[0]))
        return JS_UNDEFINED;
    cp = (dyn_cps_t*)(uintptr_t)p;
    if (cp->dead)
        return JS_UNDEFINED;
    cp->step(ctx, cp, argc > 1 ? argv[1] : JS_UNDEFINED, 0);
    return JS_UNDEFINED;
}

static void dyn_cps_fail(JSContext* ctx, dyn_cps_t* cp, dyn_cps_step_fn step)
{
    JSValue exc = stream_take_exc(ctx);
    if (JS_IsException(exc))
        return;
    step(ctx, cp, exc, 1);
    JS_FreeValue(ctx, exc);
}

void dyn_cps_await(JSContext* ctx, dyn_cps_t* cp, dyn_cps_step_fn step,
    JSValue promise)
{
    JSValue dptr, onok, onerr, pthen, tr;
    JSValueConst thenargs[2];

    dyn_cps_arm_kill(ctx, JS_GetRuntime(ctx), cp);
    if (JS_IsException(promise)) {
        dyn_cps_fail(ctx, cp, step);
        return;
    }
    {
        JSPromiseStateEnum st = JS_PromiseState(ctx, promise);
        if (st == JS_PROMISE_REJECTED) {
            JSValue pthen_v = JS_GetPropertyStr(ctx, promise, "then");
            if (!JS_IsException(pthen_v)) {
                JSValue swallow = JS_NewCFunction(ctx, stream_swallow_rejection,
                    "", 1);
                if (!JS_IsException(swallow)) {
                    JSValueConst na[2] = { JS_UNDEFINED, swallow };
                    JSValue tr_v = JS_Call(ctx, pthen_v, promise, 2, na);
                    if (JS_IsException(tr_v))
                        stream_drop_exc(ctx);
                    else
                        JS_FreeValue(ctx, tr_v);
                    JS_FreeValue(ctx, swallow);
                } else {
                    stream_drop_exc(ctx);
                }
                JS_FreeValue(ctx, pthen_v);
            } else {
                stream_drop_exc(ctx);
            }
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, promise);
                return;
            }
            {
                JSValue reason = JS_PromiseResult(ctx, promise);
                JS_FreeValue(ctx, promise);
                step(ctx, cp, reason, 1);
                JS_FreeValue(ctx, reason);
            }
            return;
        }
        if (st == JS_PROMISE_FULFILLED) {
            JSValue d, result = JS_PromiseResult(ctx, promise);
            JSValueConst jargs[2];
            int rc;
            JS_FreeValue(ctx, promise);
            cp->step = step;
            d = JS_NewInt64(ctx, (int64_t)(uintptr_t)cp);
            jargs[0] = d;
            jargs[1] = result;
            rc = JS_EnqueueJob(ctx, dyn_cps_job, 2, jargs);
            JS_FreeValue(ctx, d);
            JS_FreeValue(ctx, result);
            if (rc < 0)
                dyn_cps_fail(ctx, cp, step);
            return;
        }
    }
    dptr = JS_NewObjectClass(ctx, dyn_cps_anchor_class_id);
    if (JS_IsException(dptr)) {
        JS_FreeValue(ctx, promise);
        dyn_cps_fail(ctx, cp, step);
        return;
    }
    {
        dyn_cps_arm_t* a = (dyn_cps_arm_t*)calloc(1, sizeof *a);
        if (!a) {
            JS_FreeValue(ctx, dptr);
            JS_FreeValue(ctx, promise);
            JS_ThrowOutOfMemory(ctx);
            dyn_cps_fail(ctx, cp, step);
            return;
        }
        JS_SetOpaque(dptr, a);
        a->cp = cp;
        a->anchor = dptr;
        cp->arm = a;
    }
    onok = JS_NewCFunctionData(ctx, dyn_cps_settle, 1, 0, 1, &dptr);
    onerr = JS_NewCFunctionData(ctx, dyn_cps_settle, 1, 1, 1, &dptr);
    JS_FreeValue(ctx, dptr);
    if (JS_IsException(onok) || JS_IsException(onerr)) {
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        dyn_cps_fail(ctx, cp, step);
        return;
    }
    cp->step = step;
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    if (JS_IsException(pthen)) {
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        dyn_cps_fail(ctx, cp, step);
        return;
    }
    if (!JS_IsFunction(ctx, pthen)) {
        JS_FreeValue(ctx, pthen);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        JS_ThrowTypeError(ctx,
            "ByteSource.read: must return a Promise<number> (a pull-based"
            " read resolves its caller's byte count asynchronously)");
        dyn_cps_fail(ctx, cp, step);
        return;
    }
    thenargs[0] = onok;
    thenargs[1] = onerr;
    tr = JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onok);
    JS_FreeValue(ctx, onerr);
    if (JS_IsException(tr)) {
        dyn_cps_fail(ctx, cp, step);
    } else {
        JS_FreeValue(ctx, tr);
    }
}

#endif
