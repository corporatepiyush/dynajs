#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define JS_ToCStringLen(...) (dyn_fiber_onjs(), JS_ToCStringLen(__VA_ARGS__))

static const char* const ml_as_keys[] = { "as" };

int dyn_ml_parse_as_opt(JSContext* ctx, int argc, JSValueConst* argv,
    int from, int* pas_f64)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue v;
    size_t len;
    const char* sv;
    int ok = 0;

    *pas_f64 = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, ml_as_keys, 1))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "as");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    sv = JS_ToCStringLen(ctx, &len, v);
    if (!sv) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    ok = strcmp(sv, "f64") == 0;
    JS_FreeCString(ctx, sv);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "ml.predict: the \"as\" option is \"f64\" (a "
                               "Float64Array result) or omitted");
        return -1;
    }
    *pas_f64 = 1;
    return 0;
}

int dyn_ml_parse_jobs(JSContext* ctx, JSValueConst opts,
    size_t* pnjobs, JSValue* ponprogress)
{
    JSValue v;

    *pnjobs = 1;
    *ponprogress = JS_UNDEFINED;
    if (JS_IsUndefined(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "nJobs");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int64_t j;
        if (JS_ToInt64(ctx, &j, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (j < 1) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx,
                "dyna:ml nJobs must be at least 1; parallel execution is "
                "reserved (an os.Worker cannot receive the estimator factory, "
                "so folds currently run sequentially)");
            return -1;
        }
        *pnjobs = (size_t)j;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "onProgress");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsFunction(ctx, v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                "dyna:ml onProgress must be a function ({done, total})");
            return -1;
        }
        *ponprogress = v;
    } else {
        JS_FreeValue(ctx, v);
    }
    return 0;
}

#endif
