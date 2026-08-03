/*
 * dyna:random -- the JS binding for the seedable PRNG. The generator itself
 * is src/core/dyn-prng.c (pure C); this file is the boundary.
 *
 * UUIDs are dyna:uuid's job -- v4 lived here as a second implementation.
 *
 *   import { Random } from "dyna:random";
 *   const r = new Random(42);        // deterministic when seeded
 *   try {
 *     r.nextU64();      // BigInt in [0, 2^64)     (full 64-bit, lossless)
 *     r.nextU53();      // Number in [0, 2^53)     (top 53 bits, exact)
 *     r.nextFloat();    // Number in [0, 1)
 *     r.nextBounded(6); // uniform in [0, 6)       (unbiased rejection sampling)
 *     r.fill(new Uint8Array(16));
 *   } finally { r.close(); }
 *
 * PRNG: xoshiro256** (256-bit state) seeded through splitmix64. The native
 * object is just its 256-bit state (POD), so disposal is a single free(). The
 * unseeded constructor draws from OS entropy. Native results are
 * copied into fresh JS values at the boundary -- nothing native escapes.
 *
 * uint64 representation (type-stable): a 64-bit draw does not always fit a JS
 * Number, so nextU64() ALWAYS returns a BigInt (lossless), nextU53() is the fast
 * exact Number path, and nextBounded() mirrors its argument's type.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the pure-C generator (src/core/dyn-prng.c) */
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_RANDOM_MAX_SAFE 9007199254740992.0 /* 2^53 */

/* ---------- Random class ---------- */

static JSClassID dyn_random_class_id;

static void dyn_random_dispose(void *native)
{
    free(native);
}

/* Plain GC class: the whole native state is one 64-bit integer, so there is
 * nothing scarce to release early and no reason to make callers remember a
 * close(). The finalizer frees it when the object becomes unreachable, exactly
 * like a Map. */
static void dyn_random_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_random_dispose(JS_GetOpaque(val, dyn_random_class_id));
}

static const JSClassDef dyn_random_class = {
    "Random",
    .finalizer = dyn_random_finalizer,
};

static JSValue dyn_random_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    uint64_t seed;

    /* Resolve the seed to a C local BEFORE allocating (a throwing coercion then
     * leaks nothing). A given seed is deterministic; an omitted/undefined seed
     * is drawn from OS entropy. 42 and 42n map to the same stream. */
    (void)new_target;
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        dyn_os_entropy(&seed, sizeof(seed));
    } else if (JS_IsBigInt(ctx, argv[0])) {
        int64_t s;
        if (JS_ToBigInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    } else {
        int64_t s;
        if (JS_ToInt64(ctx, &s, argv[0]))
            return JS_EXCEPTION;
        seed = (uint64_t)s;
    }

    rng = (dyn_prng_t *)malloc(sizeof(*rng));
    if (!rng)
        return JS_ThrowOutOfMemory(ctx);
    dyn_prng_seed(rng, seed);
    return dyn_plain_wrap(ctx, dyn_random_class_id, rng, dyn_random_dispose);
}

static JSValue dyn_random_next_u64(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, dyn_prng_next(rng));
}

static JSValue dyn_random_next_u53(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(dyn_prng_next(rng) >> 11));
}

static JSValue dyn_random_next_float(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_next_double(rng));
}

static JSValue dyn_random_next_bounded(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    uint64_t bound, result;
    int is_bigint;

    /* Coerce the bound FIRST (JS_ToFloat64 may run user valueOf), THEN resolve.
     * A BigInt bound yields a BigInt result; a Number bound a Number -- the
     * return type follows the input and never truncates past 2^53. */
    (void)argc;
    is_bigint = JS_IsBigInt(ctx, argv[0]);
    if (is_bigint) {
        int64_t b;
        if (JS_ToBigInt64(ctx, &b, argv[0]))
            return JS_EXCEPTION;
        if (b <= 0)
            return JS_ThrowRangeError(ctx, "bound must be a positive integer");
        bound = (uint64_t)b;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (!(d >= 1.0) || d > DYN_RANDOM_MAX_SAFE)
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

static JSValue dyn_random_fill(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    uint8_t *base;
    size_t byte_off, byte_len, ab_size;

    /* Extract the destination buffer FIRST (no user JS runs for a real
     * TypedArray; anything else throws TypeError, so a valueOf-close attack
     * never reaches native use), THEN resolve -- nothing JS-invoking between the
     * resolve and dyn_prng_fill. Bytes land in the JS-owned ArrayBuffer. */
    (void)argc;
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, NULL);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    dyn_prng_fill(rng, base + byte_off, byte_len);
    JS_FreeValue(ctx, buf_val);
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry dyn_random_proto[] = {
    JS_CFUNC_DEF("nextU64", 0, dyn_random_next_u64),
    JS_CFUNC_DEF("nextU53", 0, dyn_random_next_u53),
    JS_CFUNC_DEF("nextFloat", 0, dyn_random_next_float),
    JS_CFUNC_DEF("nextBounded", 1, dyn_random_next_bounded),
    JS_CFUNC_DEF("fill", 1, dyn_random_fill),
};

/* ---------- module registration ---------- */

static int dyn_random_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_random_class_id, &dyn_random_class,
                           dyn_random_proto, countof(dyn_random_proto),
                           dyn_random_ctor, "Random") < 0)
        return -1;
    return 0;
}

int js_nat_init_random(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:random", dyn_random_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Random");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_RANDOM */
