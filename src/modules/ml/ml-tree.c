#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static int dyn_tree_poll(void* ud)
{
    return JS_CheckInterrupt((JSContext*)ud);
}

int dyn_tree_fit_one(JSContext* ctx, dyn_tree_t* t, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* classes, size_t n_classes,
    const dyn_tree_opts_t* opt, uint64_t* rng,
    uint32_t* idx, size_t n, const dyn_tree_work_t* w)
{
    int interrupted = 0;
    if (dyn_tree_fit(t, X, y, rows, cols, classes, n_classes, opt, rng, idx, n, w,
            dyn_tree_poll, ctx, &interrupted)
        != 0) {
        if (!interrupted)
            JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue dyn_ml_idx_array(JSContext* ctx, const uint32_t* v, size_t n)
{
    JSValue a = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i,
                JS_NewUint32(ctx, v[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

JSValue dyn_ml_split_obj(JSContext* ctx, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte)
{
    JSValue o = JS_NewObject(ctx), a;
    if (JS_IsException(o))
        return o;
    a = dyn_ml_idx_array(ctx, tr, ntr);
    if (JS_IsException(a) || JS_DefinePropertyValueStr(ctx, o, "train", a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    a = dyn_ml_idx_array(ctx, te, nte);
    if (JS_IsException(a) || JS_DefinePropertyValueStr(ctx, o, "test", a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

#endif
