#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

#define DYN_RANDOM_MAX_SAFE 9007199254740992.0

JSValue dyn_random_next_u64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_prng_next(rng));
}

JSValue dyn_random_next_u53(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(dyn_prng_next(rng) >> 11));
}

JSValue dyn_random_next_float(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc;
    (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_next_double(rng));
}

JSValue dyn_random_next_bounded(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    uint64_t bound, result;
    int is_bigint;

    (void)argc;
    is_bigint = JS_IsBigInt(ctx, argv[0]);
    if (is_bigint) {
        int64_t b;
        if (JS_ToBigInt64(ctx, &b, argv[0]))
            return JS_EXCEPTION;
        if (b == 0)
            return JS_ThrowRangeError(ctx, "bound must be a positive integer");
        bound = (uint64_t)b;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (!(d >= 1.0) || d > DYN_RANDOM_MAX_SAFE || d != floor(d))
            return JS_ThrowRangeError(ctx,
                "bound must be an integer in [1, 2^53] (use a BigInt for more)");
        bound = (uint64_t)d;
    }

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    result = dyn_prng_next_bounded(rng, bound);
    return is_bigint ? JS_NewBigUint64(ctx, result)
                     : JS_NewInt64(ctx, (int64_t)result);
}

#endif
