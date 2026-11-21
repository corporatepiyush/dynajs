#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define dyn_res_hold(...) (dyn_fiber_onjs(), dyn_res_hold(__VA_ARGS__))
#define JS_GetRuntime(...) (dyn_fiber_onjs(), JS_GetRuntime(__VA_ARGS__))

typedef struct {
    JSValue* stage;
    size_t n;
    int fitted;
    JSRuntime* rt;
} dyn_pipe_t;

static void dyn_pipe_dispose(void* native)
{
    dyn_pipe_t* pp = (dyn_pipe_t*)native;
    size_t i;
    if (!pp)
        return;
    for (i = 0; i < pp->n; i++)
        JS_FreeValueRT(pp->rt, pp->stage[i]);
    free(pp->stage);
    free(pp);
}

void dyn_pipe_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_pipe_class_id);
    dyn_pipe_t* pp = r ? (dyn_pipe_t*)r->native : NULL;
    size_t i;
    if (!pp || !pp->stage)
        return;
    for (i = 0; i < pp->n; i++)
        JS_MarkValue(rt, pp->stage[i], mark_func);
}

JSValue dyn_pipe_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp;
    int64_t len = 0, i;
    JSValue ret;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "ml.Pipeline: new Pipeline(stages[]) requires an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (len < 1)
        return JS_ThrowRangeError(ctx, "ml.Pipeline: a Pipeline needs at least one stage");
    if (len > 64)
        return JS_ThrowRangeError(ctx, "ml.Pipeline: at most 64 stages");

    pp = (dyn_pipe_t*)calloc(1, sizeof(*pp));
    if (!pp)
        return JS_ThrowOutOfMemory(ctx);
    pp->rt = JS_GetRuntime(ctx);
    pp->stage = (JSValue*)calloc((size_t)len, sizeof(JSValue));
    if (!pp->stage) {
        free(pp);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < len; i++) {
        JSValue st = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue fn;
        if (JS_IsException(st))
            goto fail;
        if (!JS_IsObject(st)) {
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "ml.Pipeline: Pipeline stage %d is not an object", (int)i);
            goto fail;
        }
        fn = JS_GetPropertyStr(ctx, st, "fit");
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "ml.Pipeline: Pipeline stage %d has no fit()", (int)i);
            goto fail;
        }
        JS_FreeValue(ctx, fn);
        if (i < len - 1) {
            fn = JS_GetPropertyStr(ctx, st, "transform");
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, st);
                JS_ThrowTypeError(ctx,
                    "ml.Pipeline: Pipeline stage %d has no transform(); only the LAST stage "
                    "may be a bare estimator",
                    (int)i);
                goto fail;
            }
            JS_FreeValue(ctx, fn);
        }
        pp->stage[i] = st;
        pp->n = (size_t)(i + 1);
    }
    ret = dyn_res_wrap(ctx, new_target, dyn_pipe_class_id, pp, dyn_pipe_dispose);
    if (JS_IsException(ret))
        return ret;
    return ret;

fail:
    for (i = 0; i < (int64_t)pp->n; i++)
        JS_FreeValue(ctx, pp->stage[i]);
    free(pp->stage);
    free(pp);
    return JS_EXCEPTION;
}

static JSValue dyn_pipe_call(JSContext* ctx, JSValueConst obj, const char* name,
    int n, JSValueConst* args)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name);
    JSValue r;
    if (JS_IsException(fn))
        return fn;
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return JS_ThrowTypeError(ctx, "ml.Pipeline: a stage has no %s()", name);
    }
    r = JS_Call(ctx, fn, obj, n, args);
    JS_FreeValue(ctx, fn);
    return r;
}

static dyn_pipe_t* dyn_pipe_of(JSContext* ctx, JSValueConst t)
{
    DynResource* r = dyn_res_get(ctx, t, dyn_pipe_class_id);
    if (!r)
        return NULL;
    dyn_res_hold(ctx, r);
    return (dyn_pipe_t*)r->native;
}

JSValue dyn_pipe_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y?) requires X");
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "Pipeline.fit (set it on the estimator)"))
        return JS_EXCEPTION;

    cur = JS_DupValue(ctx, argv[0]);
    for (i = 0; i + 1 < pp->n; i++) {
        JSValueConst a[1];
        JSValue next;
        a[0] = cur;
        next = dyn_pipe_call(ctx, pp->stage[i], "fit", 1, a);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, cur);
            return next;
        }
        JS_FreeValue(ctx, next);
        next = dyn_pipe_call(ctx, pp->stage[i], "transform", 1, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(next))
            return next;
        cur = next;
    }
    {
        JSValueConst a[2];
        JSValue r;
        int n = 1;
        a[0] = cur;
        if (argc > 1) {
            a[1] = argv[1];
            n = 2;
        }
        r = dyn_pipe_call(ctx, pp->stage[pp->n - 1], "fit", n, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    pp->fitted = 1;
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_pipe_apply(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i, upto;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:ml X is required");
    if (!pp->fitted)
        return dyn_ml_throw_unfitted(ctx, this_val,
            magic == 0 ? "predict" : (magic == 1 ? "predictProba" : "transform"));

    upto = pp->n - 1;
    if (magic == 2) {
        JSValue fn = JS_GetPropertyStr(ctx, pp->stage[pp->n - 1], "transform");
        int can = JS_IsFunction(ctx, fn);
        JS_FreeValue(ctx, fn);
        if (can)
            upto = pp->n;
    }
    cur = JS_DupValue(ctx, argv[0]);
    for (i = 0; i < upto; i++) {
        JSValueConst a[1];
        JSValue next;
        a[0] = cur;
        next = dyn_pipe_call(ctx, pp->stage[i], "transform", 1, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(next))
            return next;
        cur = next;
    }
    if (magic == 2)
        return cur;
    {
        JSValueConst a[1];
        JSValue r;
        a[0] = cur;
        r = dyn_pipe_call(ctx, pp->stage[pp->n - 1],
            magic ? "predictProba" : "predict", 1, a);
        JS_FreeValue(ctx, cur);
        return r;
    }
}

JSValue dyn_pipe_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    if (!pp)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)pp->n);
    if (magic == 1)
        return JS_NewBool(ctx, pp->fitted);
    return JS_DupValue(ctx, pp->stage[pp->n - 1]);
}

JSValue dyn_pipe_stage(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    int64_t i;
    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &i, argv[0]))
        return JS_ThrowTypeError(ctx, "ml.stage: stage(i) requires an index");
    if (i < 0)
        i += (int64_t)pp->n;
    if (i < 0 || i >= (int64_t)pp->n)
        return JS_ThrowRangeError(ctx, "ml.stage: stage index out of range");
    return JS_DupValue(ctx, pp->stage[i]);
}

#endif
