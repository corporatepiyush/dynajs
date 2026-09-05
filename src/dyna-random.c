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
#include <math.h>

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
        if (dyn_os_entropy(&seed, sizeof(seed)) < 0)
            return JS_ThrowInternalError(ctx,
                "new Random(): OS entropy unavailable");
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
        /* JS_ToBigInt64 reduces mod 2^64, so a bound in [2^63, 2^64) arrives
         * NEGATIVE here. A BigInt input is exact and cannot express a bound
         * >= 2^64, so the residue IS the u64 the caller wrote: take the bit
         * pattern instead of throwing on the sign. (By that same reduction
         * -5n aliases 2^64-5n -- the identity ToBigInt64 defines.) Zero is
         * still refused: the core's rejection sampler divides by it. */
        if (b == 0)
            return JS_ThrowRangeError(ctx, "bound must be a positive integer");
        bound = (uint64_t)b;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        /* 6.5 used to pass this gate and truncate to 6 -- a silently biased
           die. A non-integral bound is refused, not floored. */
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
    /* d.ts: fill(...): this — chainable like the other instance methods */
    return JS_DupValue(ctx, this_val);
}

/* ---------- state checkpoint / resume (getState / setState) ---------- */

/* The state travels as a 32-byte Uint8Array: the four 64-bit words s[0..3],
 * each little-endian. The byte layout is fixed (not the host's memory image)
 * so a checkpoint survives a round-trip through storage or the network. */
static JSValue dyn_random_get_state(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    uint8_t raw[32];
    JSValue ab, out;
    JSValueConst ta_args[3];
    int i, j;
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    for (i = 0; i < 4; i++) {
        uint64_t w = rng->s[i];
        for (j = 0; j < 8; j++)
            raw[i * 8 + j] = (uint8_t)(w >> (8 * j));
    }
    ab = JS_NewArrayBufferCopy(ctx, raw, sizeof raw);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_random_set_state(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    uint8_t *base;
    size_t byte_off, byte_len, ab_size;
    uint64_t w[4];
    int i, j, all_zero = 1;

    /* Resolve the incoming view FIRST (same discipline as fill(): no user JS
     * runs between the resolve and the state write), THEN the generator. */
    (void)argc;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "setState(state): a 32-byte Uint8Array from getState() is required");
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, NULL);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off || byte_len != 32) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx,
            "setState(state): the state is exactly 32 bytes (four little-endian u64 words)");
    }
    for (i = 0; i < 4; i++) {
        w[i] = 0;
        for (j = 7; j >= 0; j--)
            w[i] = (w[i] << 8) | base[byte_off + i * 8 + j];
        if (w[i])
            all_zero = 0;
    }
    JS_FreeValue(ctx, buf_val);
    if (all_zero)
        return JS_ThrowRangeError(ctx,
            "setState(state): the all-zero state is xoshiro256**'s fixed point and would emit zeros forever");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    for (i = 0; i < 4; i++)
        rng->s[i] = w[i];
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_random_proto[] = {
    JS_CFUNC_DEF("nextU64", 0, dyn_random_next_u64),
    JS_CFUNC_DEF("nextU53", 0, dyn_random_next_u53),
    JS_CFUNC_DEF("nextFloat", 0, dyn_random_next_float),
    JS_CFUNC_DEF("nextBounded", 1, dyn_random_next_bounded),
    JS_CFUNC_DEF("fill", 1, dyn_random_fill),
    JS_CFUNC_DEF("getState", 0, dyn_random_get_state),
    JS_CFUNC_DEF("setState", 1, dyn_random_set_state),
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
