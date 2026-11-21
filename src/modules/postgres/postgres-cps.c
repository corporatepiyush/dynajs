#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
void pg_cps_defuse_all(JSContext* ctx, pg_cps_t* cp)
{
    pg_cps_ref_t* r = cp->refs;
    while (r) {
        pg_cps_ref_t* n = r->next;
        JS_SetOpaque(r->obj, NULL);
        JS_FreeValue(ctx, r->obj);
        free(r);
        r = n;
    }
    cp->refs = NULL;
}

static int pg_cps_track(JSContext* ctx, pg_cps_t* cp, JSValue obj)
{
    pg_cps_ref_t* r = (pg_cps_ref_t*)malloc(sizeof(*r));
    if (!r) {
        JS_FreeValue(ctx, obj);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    r->obj = obj;
    r->next = cp->refs;
    cp->refs = r;
    return 0;
}

static int pg_cps_new_cont(JSContext* ctx, pg_cps_t* cp, JSValue* out)
{
    JSValue cobj = JS_NewObjectClass(ctx, (int)pgcps_class_id);
    if (JS_IsException(cobj))
        return -1;
    JS_SetOpaque(cobj, cp);
    if (pg_cps_track(ctx, cp, JS_DupValue(ctx, cobj)) < 0) {
        JS_FreeValue(ctx, cobj);
        return -1;
    }
    *out = cobj;
    return 0;
}

static JSValue pg_cps_take(JSContext* ctx)
{
    int unc = JS_HasUncatchableException(ctx);
    JSValue exc = JS_GetException(ctx);
    if (unc)
        js_std_set_job_error_rt(JS_GetRuntime(ctx), JS_DupValue(ctx, exc));
    return exc;
}

static JSValue pg_swallow_rejection(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static JSValue pg_cps_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValueConst* func_data)
{
    pg_cps_t* cp;

    (void)this_val;
    (void)argc;
    cp = (pg_cps_t*)JS_GetOpaque(func_data[0], pgcps_class_id);
    if (!cp || cp->settled)
        return JS_UNDEFINED;
    cp->settled = 1;
    cp->step(ctx, cp, argc > 0 ? argv[0] : JS_UNDEFINED, magic);
    return JS_UNDEFINED;
}

static JSValue pg_cps_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    pg_cps_t* cp;

    cp = (pg_cps_t*)JS_GetOpaque(argv[0], pgcps_class_id);
    if (!cp)
        return JS_UNDEFINED;
    cp->step(ctx, cp, argc > 1 ? argv[1] : JS_UNDEFINED, 0);
    return JS_UNDEFINED;
}

void pg_cps_await(JSContext* ctx, pg_cps_t* cp, pg_cps_step_fn step,
    JSValue promise)
{
    JSValue cobj, onok, onerr, pthen, tr;
    JSValueConst thenargs[2];

    cp->settled = 0;
    if (JS_IsException(promise)) {
        JSValue exc = pg_cps_take(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    {
        JSPromiseStateEnum st = JS_PromiseState(ctx, promise);
        if (st == JS_PROMISE_REJECTED) {
            JSValue pthen2 = JS_GetPropertyStr(ctx, promise, "then");
            if (JS_IsException(pthen2)) {
                dyn_exc_drop(ctx);
            } else if (JS_IsFunction(ctx, pthen2)) {
                JSValue sw = JS_NewCFunction(ctx, pg_swallow_rejection, "", 1);
                JSValueConst na[2] = { JS_UNDEFINED, sw };
                JSValue tr2 = JS_Call(ctx, pthen2, promise, 2, na);
                if (JS_IsException(tr2))
                    dyn_exc_drop(ctx);
                else
                    JS_FreeValue(ctx, tr2);
                JS_FreeValue(ctx, sw);
            }
            JS_FreeValue(ctx, pthen2);
            {
                JSValue reason = JS_PromiseResult(ctx, promise);
                JS_FreeValue(ctx, promise);
                step(ctx, cp, reason, 1);
                JS_FreeValue(ctx, reason);
            }
            return;
        }
        if (st == JS_PROMISE_FULFILLED) {
            JSValue result = JS_PromiseResult(ctx, promise);
            JSValueConst jargs[2];
            int rc;
            JS_FreeValue(ctx, promise);
            cp->step = step;
            if (pg_cps_new_cont(ctx, cp, &cobj) < 0) {
                JS_FreeValue(ctx, result);
                {
                    JSValue exc = pg_cps_take(ctx);
                    step(ctx, cp, exc, 1);
                    JS_FreeValue(ctx, exc);
                }
                return;
            }
            jargs[0] = cobj;
            jargs[1] = result;
            rc = JS_EnqueueJob(ctx, pg_cps_job, 2, jargs);
            JS_FreeValue(ctx, cobj);
            JS_FreeValue(ctx, result);
            if (rc < 0) {
                JSValue exc = pg_cps_take(ctx);
                step(ctx, cp, exc, 1);
                JS_FreeValue(ctx, exc);
            }
            return;
        }
    }
    if (pg_cps_new_cont(ctx, cp, &cobj) < 0) {
        JSValue exc = pg_cps_take(ctx);
        JS_FreeValue(ctx, promise);
        if (JS_IsNull(exc))
            exc = JS_ThrowOutOfMemory(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    onok = JS_NewCFunctionData(ctx, pg_cps_settle, 1, 0, 1, &cobj);
    onerr = JS_NewCFunctionData(ctx, pg_cps_settle, 1, 1, 1, &cobj);
    JS_FreeValue(ctx, cobj);
    if (JS_IsException(onok) || JS_IsException(onerr)) {
        JSValue exc = pg_cps_take(ctx);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    cp->step = step;
    pthen = JS_GetPropertyStr(ctx, promise, "then");
    if (JS_IsException(pthen) || !JS_IsFunction(ctx, pthen)) {
        JSValue exc;
        if (!JS_IsException(pthen))
            JS_ThrowTypeError(ctx, "queryIter: query() returned a non-promise");
        JS_FreeValue(ctx, pthen);
        JS_FreeValue(ctx, onok);
        JS_FreeValue(ctx, onerr);
        JS_FreeValue(ctx, promise);
        exc = pg_cps_take(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    thenargs[0] = onok;
    thenargs[1] = onerr;
    tr = JS_Call(ctx, pthen, promise, 2, thenargs);
    JS_FreeValue(ctx, pthen);
    JS_FreeValue(ctx, onok);
    JS_FreeValue(ctx, onerr);
    JS_FreeValue(ctx, promise);
    if (JS_IsException(tr)) {
        JSValue exc = pg_cps_take(ctx);
        step(ctx, cp, exc, 1);
        JS_FreeValue(ctx, exc);
        return;
    }
    JS_FreeValue(ctx, tr);
}
#endif
