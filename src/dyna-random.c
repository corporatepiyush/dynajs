/*
 * dyna:random -- the JS binding for the seedable PRNG. The generator itself
 * is src/core/dyn-prng.c (pure C); this file is the boundary.
 *
 * UUIDs are dyna:uuid's job -- v4 lived here as a second implementation.
 *
 *   import { Random } from "dyna:random";
 *   const r = new Random(42);        // deterministic when seeded
 *   r.nextU64();      // BigInt in [0, 2^64)     (full 64-bit, lossless)
 *   r.nextU53();      // Number in [0, 2^53)     (top 53 bits, exact)
 *   r.nextFloat();    // Number in [0, 1)
 *   r.nextBounded(6); // uniform in [0, 6)       (unbiased rejection sampling)
 *   r.normal(10, 2); // Normal(10, 2) (Marsaglia polar)
 *   r.exponential(4); // Exp(4)                  (inverse CDF)
 *   r.poisson(30);    // exact integer           (Knuth / centered walk)
 *   r.shuffle(a); // in-place Fisher-Yates (seeded)
 *   r.sample(a, 5);   // 5 draws without replacement
 *   r.choice(a);      // one uniform element
 *   r.fill(u8, 4, 8); // bytes 4..11 of u8 (window)
 *   r.bytes(16);      // fresh Uint8Array
 *   r.jump; // independent stream (canonical 2^128)
 *   // no close(): the state is GC-reclaimed like a Map's (see below)
 *
 * PRNG: xoshiro256** (256-bit state) seeded through splitmix64. The native
 * object is just its 256-bit state (POD), so disposal is a single free(). The
 * unseeded constructor draws from OS entropy. Native results are
 * copied into fresh JS values at the boundary -- nothing native escapes.
 *
 * uint64 representation (type-stable): a 64-bit draw does not always fit a JS
 * Number, so nextU64() ALWAYS returns a BigInt (lossless), nextU53() is the
 * fast exact Number path, and nextBounded() mirrors its argument's type.
 *
 * STREAM DISCIPLINE: every draw comes from the instance's stream in
 * a fixed order determined only by the seed, so a seeded Random reproduces
 * its sequences exactly -- including through normal/exponential/poisson/
 * shuffle/sample/choice, whose per-call draw COUNTS are fixed (normal: 2 per
 * accepted pair attempt; exponential: 1; poisson: 1 above lambda<30's
 * variable count; shuffle: len-1; sample: n; choice: 1). Validation happens
 * BEFORE the first draw of a call: a refused argument consumes nothing.
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
#define DYN_RANDOM_POISSON_MAX 2147483647.0    /* 2^31-1: keeps the walk O(sqrt) */
#define DYN_RANDOM_BYTES_MAX 1073741824.0      /* 2^30 = 1 GiB: checked BEFORE the allocation */

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
    return dyn_plain_wrap(ctx, new_target, dyn_random_class_id, rng, dyn_random_dispose);
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

/* ---------- argument helpers ---------- */

/* Coerces argv[i] to a finite double in [lo, hi]. An omitted / undefined
 * argument takes `def`. NaN (fails the >= test) and infinities throw
 * RangeError. Returns 0 or -1 (throwing).
 * NOTE: JS_ToFloat64 may run a user valueOf -- callers must (re)resolve any
 * held buffer pointer AFTER this returns. */
static int dyn_random_arg_num(JSContext *ctx, JSValueConst v, int has_arg,
                              double def, double lo, double hi,
                              double *out, const char *what)
{
    double d;

    if (!has_arg || JS_IsUndefined(v)) {
        *out = def;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo) || d > hi || !isfinite(d)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number in [%#g, %#g]",
                           what, lo, hi);
        return -1;
    }
    *out = d;
    return 0;
}

/* Real-valued arg with NO artificial range cap: any finite double passes
 * (normal's mu may legitimately be 1e300); NaN and infinities refuse via the
 * helper's isfinite gate. Callers add their own sign constraints. */
static int dyn_random_arg_real(JSContext *ctx, JSValueConst v, int has_arg,
                               double def, double *out, const char *what)
{
    double d = def;

    if (!has_arg || JS_IsUndefined(v)) {
        *out = def;
        return 0;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number", what);
        return -1;
    }
    *out = d;
    return 0;
}

/* Integer-only variant (fill offsets, sample n, bytes n): a non-integral
 * value like 6.5 is refused, not floored. */
static int dyn_random_arg_int(JSContext *ctx, JSValueConst v, int has_arg,
                              double def, double lo, double hi,
                              double *out, const char *what)
{
    double d = *out;

    if (dyn_random_arg_num(ctx, v, has_arg, def, lo, hi, &d, what))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "%s must be an integer", what);
        return -1;
    }
    *out = d;
    return 0;
}

/* ----------: distributions over the instance stream ---------- */

static JSValue dyn_random_normal(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    double mu = 0.0, sigma = 1.0;

    /* mu/sigma are REALS, not integers: normal(2.5, 3) is the point, and any
     * finite mu is legal (results may still overflow to +-Infinity under
     * IEEE -- normal(1e308, 1) is a finite argument, not an error) */
    if (dyn_random_arg_real(ctx, argv[0], argc > 0, 0.0,
                            &mu, "normal(mu, sigma): mu") ||
        dyn_random_arg_real(ctx, argv[1], argc > 1, 1.0,
                            &sigma, "normal(mu, sigma): sigma"))
        return JS_EXCEPTION;
    /* sigma is a scale, not a count: -3 and NaN are refused, but sigma=0 is
     * the degenerate point mass at mu and returns it without consuming draws */
    if (sigma < 0.0)
        return JS_ThrowRangeError(ctx,
            "normal(mu, sigma): sigma must be >= 0");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    if (sigma == 0.0)
        return JS_NewFloat64(ctx, mu);
    return JS_NewFloat64(ctx, dyn_prng_normal(rng, mu, sigma));
}

static JSValue dyn_random_exponential(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    double lambda = 1.0;

    if (dyn_random_arg_real(ctx, argv[0], argc > 0, 1.0,
                            &lambda, "exponential(lambda): lambda"))
        return JS_EXCEPTION;
    /* the inverse CDF divides by lambda: 0 and negatives are refused; a
       sub-normal lambda may still yield +Infinity (IEEE, documented) */
    if (!(lambda > 0.0))
        return JS_ThrowRangeError(ctx,
            "exponential(lambda): lambda must be a finite number > 0");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, dyn_prng_exponential(rng, lambda));
}

static JSValue dyn_random_poisson(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    double lambda;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "poisson(lambda): lambda is required");
    if (dyn_random_arg_num(ctx, argv[0], 1, 0.0,
                           0.0, DYN_RANDOM_POISSON_MAX,
                           &lambda, "poisson(lambda): lambda"))
        return JS_EXCEPTION;

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    /* a non-negative exact integer < 2^31+2^18: Number-safe */
    return JS_NewInt64(ctx, dyn_prng_poisson(rng, lambda));
}

/* ----------: jump / longJump (independent streams) ---------- */

static JSValue dyn_random_jump(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    /* mutate-in-place and return this: the pre-jump state stays reachable by
     * getState() discipline (copy before jumping) rather than by allocation */
    dyn_prng_jump(rng);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_random_long_jump(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    dyn_prng_t *rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    (void)argc; (void)argv;
    if (!rng)
        return JS_EXCEPTION;
    dyn_prng_long_jump(rng);
    return JS_DupValue(ctx, this_val);
}

/* ----------: fill with a window + fresh bytes ---------- */

static JSValue dyn_random_fill(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    uint8_t *base;
    size_t byte_off, byte_len, bpe, ab_size, count;
    double offset = 0.0, length;
    size_t fill_off, fill_len;

    /* Classify the destination FIRST (a real TypedArray runs no user JS;
     * anything else throws TypeError before any draw), then coerce the
     * window -- WHICH MAY RUN USER JS (valueOf) -- and only then re-fetch the
     * base pointer, so a valueOf-side detach cannot leave us filling a
     * dangling buffer. */
    (void)argc;
    buf_val = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, &bpe);
    if (JS_IsException(buf_val))
        return JS_EXCEPTION;
    count = byte_len / bpe;

    if (dyn_random_arg_int(ctx, argv[1], argc > 1, 0.0,
                           0.0, (double)count,
                           &offset, "fill(buf, offset, length): offset")) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    if (dyn_random_arg_int(ctx, argv[2], argc > 2, (double)count - offset,
                           0.0, (double)count - offset,
                           &length, "fill(buf, offset, length): length")) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;
    }
    base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
    if (!base) {
        JS_FreeValue(ctx, buf_val);
        return JS_EXCEPTION;    /* detached by a valueOf side effect, or OOM */
    }
    if (byte_off > ab_size || byte_len > ab_size - byte_off) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    fill_off = byte_off + (size_t)offset * bpe;
    fill_len = (size_t)length * bpe;
    if (fill_off > ab_size || fill_len > ab_size - fill_off) {
        JS_FreeValue(ctx, buf_val);
        return JS_ThrowRangeError(ctx, "typed array out of bounds");
    }
    dyn_prng_fill(rng, base + fill_off, fill_len);
    JS_FreeValue(ctx, buf_val);
    /* d.ts: fill(...): this -- chainable like the other instance methods */
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_random_bytes(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue ab, out, ta_args[3];
    uint8_t *buf;
    double dn;
    size_t n;

    (void)argc;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "bytes(n): n is required");
    if (JS_ToFloat64(ctx, &dn, argv[0]))
        return JS_EXCEPTION;
    if (!(dn >= 0.0) || dn > DYN_RANDOM_BYTES_MAX || dn != floor(dn))
        return JS_ThrowRangeError(ctx,
            "bytes(n): n must be an integer in [0, 2^30] (1 GiB cap)");

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    n = (size_t)dn;
    buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    dyn_prng_fill(rng, buf, n);   /* n == 0 consumes no draws */
    ab = JS_NewArrayBufferCopy(ctx, buf, n);
    free(buf);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

/* ----------: shuffle / sample / choice ---------- */

/* Classify the collection argument: 1 = typed array (its buffer view lands in
 * buf_out -- the caller frees it -- with dims), 0 = not a typed array (the
 * pending TypeError is CLEARED; the caller may try the JS-array path).
 * Never throws. */
static int dyn_random_view_ta(JSContext *ctx, JSValueConst v, JSValue *buf_out,
                              size_t *byte_off, size_t *byte_len, size_t *bpe)
{
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, byte_off, byte_len, bpe);
    if (!JS_IsException(buf)) {
        *buf_out = buf;            /* caller owns this reference */
        return 1;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));   /* clear: not a TypedArray */
    return 0;
}

/* Element count of a JS array (or array-like). -1 = throwing. */
static int dyn_random_array_len(JSContext *ctx, JSValueConst arr, double *len)
{
    JSValue lv;
    double d;

    if (!JS_IsObject(arr)) {
        JS_ThrowTypeError(ctx, "expected an Array or a TypedArray");
        return -1;
    }
    lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lv))
        return -1;
    if (JS_ToFloat64(ctx, &d, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (!(d >= 0.0) || d > 4294967295.0 || d != floor(d)) {
        JS_ThrowTypeError(ctx, "expected an Array or a TypedArray");
        return -1;
    }
    *len = d;
    return 0;
}

/* Swap arr[i] <-> arr[j] as JS values, HOLES PRESERVED: an index with no own
 * element before the swap has none after (a hole moves as a hole instead of
 * densifying into undefined). Returns 0 / -1 (throwing). Exotic index
 * accessors may run user JS; the draw COUNT of the caller's loop is fixed
 * either way (it draws before calling this). */
static int dyn_random_arr_swap(JSContext *ctx, JSValueConst arr,
                               uint32_t i, uint32_t j)
{
    JSAtom ai = JS_NewAtomUInt32(ctx, i);
    JSAtom aj = JS_NewAtomUInt32(ctx, j);
    JSPropertyDescriptor di, dj;
    int has_i, has_j, r = -1;

    if (ai == JS_ATOM_NULL || aj == JS_ATOM_NULL)
        goto out;
    has_i = JS_GetOwnProperty(ctx, &di, arr, ai);
    if (has_i < 0)
        goto out;
    has_j = JS_GetOwnProperty(ctx, &dj, arr, aj);
    if (has_j < 0) {
        if (has_i) {
            JS_FreeValue(ctx, di.value);
            JS_FreeValue(ctx, di.getter);
            JS_FreeValue(ctx, di.setter);
        }
        goto out;
    }
    /* presence-only: the descriptor values are not kept (reads below go
     * through JS_GetPropertyUint32, which returns a fresh reference) */
    if (has_i) {
        JS_FreeValue(ctx, di.value);
        JS_FreeValue(ctx, di.getter);
        JS_FreeValue(ctx, di.setter);
    }
    if (has_j) {
        JS_FreeValue(ctx, dj.value);
        JS_FreeValue(ctx, dj.getter);
        JS_FreeValue(ctx, dj.setter);
    }

    if (has_i && has_j) {
        JSValue vi = JS_GetPropertyUint32(ctx, arr, i);
        JSValue vj;
        if (JS_IsException(vi))
            goto out;
        vj = JS_GetPropertyUint32(ctx, arr, j);
        if (JS_IsException(vj)) {
            JS_FreeValue(ctx, vi);
            goto out;
        }
        r = JS_SetPropertyUint32(ctx, arr, j, vi);   /* consumes vi */
        if (r < 0) {
            JS_FreeValue(ctx, vj);
            goto out;
        }
        r = JS_SetPropertyUint32(ctx, arr, i, vj);   /* consumes vj */
    } else if (has_i) {
        /* i holds a value, j is a hole: the value moves to j, i goes dark */
        JSValue vi = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(vi))
            goto out;
        r = JS_SetPropertyUint32(ctx, arr, j, vi);   /* consumes vi */
        if (r < 0)
            goto out;
        r = JS_DeleteProperty(ctx, arr, ai, 0);
    } else if (has_j) {
        JSValue vj = JS_GetPropertyUint32(ctx, arr, j);
        if (JS_IsException(vj))
            goto out;
        r = JS_SetPropertyUint32(ctx, arr, i, vj);   /* consumes vj */
        if (r < 0)
            goto out;
        r = JS_DeleteProperty(ctx, arr, aj, 0);
    } else {
        r = 0;                    /* two holes swap into two holes */
    }
out:
    if (ai != JS_ATOM_NULL)
        JS_FreeAtom(ctx, ai);
    if (aj != JS_ATOM_NULL)
        JS_FreeAtom(ctx, aj);
    /* normalize: the setters above report success as 1; this helper's
     * contract (like JS_SetProperty-style callers expect) is 0 / -1 */
    return r < 0 ? -1 : 0;
}

/* Fisher-Yates over a JS array (in place): for i from len-1 down to 1 pick
 * j uniform in [0, i] and swap. Exactly len-1 draws for len >= 2. */
static int dyn_random_fy_array(JSContext *ctx, JSValueConst arr,
                               dyn_prng_t *rng, uint32_t len)
{
    uint32_t i;
    for (i = len - 1; i >= 1; i--) {
        uint32_t j = (uint32_t)dyn_prng_next_bounded(rng, (uint64_t)i + 1);
        if (i == j)
            continue;             /* self-swap: no property traffic at all */
        if (dyn_random_arr_swap(ctx, arr, i, j))
            return -1;
    }
    return 0;
}

/* Fisher-Yates over a typed array's bytes: same draw schedule, element swaps
 * are bpe-wide memmoves (representation is self-consistent within one array).
 * Exactly len-1 draws for len >= 2, none for len < 2. */
static void dyn_random_fy_ta(dyn_prng_t *rng, uint8_t *base,
                             size_t byte_off, size_t bpe, uint64_t len)
{
    uint8_t tmp[8];
    uint64_t i;
    if (len < 2)
        return;                   /* nothing to shuffle, no draws */
    for (i = len - 1; i >= 1; i--) {
        uint64_t j = dyn_prng_next_bounded(rng, i + 1);
        if (i == j)
            continue;
        memcpy(tmp, base + byte_off + i * bpe, bpe);
        /* i != j here, so the two element slots never overlap -- but both
         * operands root at the same buffer and the swap is a move, so use
         * memmove and let the compiler prove nothing about disjointness */
        memmove(base + byte_off + i * bpe, base + byte_off + j * bpe, bpe);
        memcpy(base + byte_off + j * bpe, tmp, bpe);
    }
}

static JSValue dyn_random_shuffle(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    size_t byte_off, byte_len, bpe;
    double len;
    int is_ta;

    (void)argc;
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;

    /* validate EVERYTHING (including the frozen refusal) before the first
     * draw: a refused call consumes nothing from the stream */
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        uint8_t *base;
        size_t ab_size;
        if (!JS_IsExtensible(ctx, argv[0])) {
            JS_FreeValue(ctx, buf_val);
            return JS_ThrowTypeError(ctx,
                "shuffle: refusing a non-extensible typed array (frozen or sealed)");
        }
        base = JS_GetArrayBuffer(ctx, &ab_size, buf_val);
        if (!base) {
            JS_FreeValue(ctx, buf_val);
            return JS_EXCEPTION;
        }
        if (byte_off > ab_size || byte_len > ab_size - byte_off) {
            JS_FreeValue(ctx, buf_val);
            return JS_ThrowRangeError(ctx, "typed array out of bounds");
        }
        dyn_random_fy_ta(rng, base, byte_off, bpe, byte_len / bpe);
        JS_FreeValue(ctx, buf_val);
    } else {
        if (dyn_random_array_len(ctx, argv[0], &len))
            return JS_EXCEPTION;
        if (!JS_IsExtensible(ctx, argv[0]))
            return JS_ThrowTypeError(ctx,
                "shuffle: refusing a non-extensible array (frozen or sealed)");
        if (len >= 2.0 && dyn_random_fy_array(ctx, argv[0], rng,
                                              (uint32_t)len))
            return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, this_val);
}

/* The shared sample core: draw n indices without replacement by partial
 * Fisher-Yates over an index table (exactly n draws), then gather. The
 * source is never mutated. Returns 0 / -1 (throwing); idx holds the picked
 * source positions in result order. */
static uint32_t *dyn_random_sample_indices(dyn_prng_t *rng, uint64_t len,
                                           uint64_t n)
{
    uint32_t *idx = (uint32_t *)malloc(len * sizeof(uint32_t));
    uint64_t i;
    if (!idx)
        return NULL;
    for (i = 0; i < len; i++)
        idx[i] = (uint32_t)i;
    for (i = 0; i < n; i++) {
        uint64_t j = i + dyn_prng_next_bounded(rng, len - i);
        uint32_t t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
    }
    return idx;
}

/* The JS-facing half of sample(): build the result array (the source's own
 * constructor for a typed array, a plain Array otherwise) and copy the
 * picked elements over. EVERY call here runs user JS (constructor, element
 * getters); it is factored out so the raw-handle window in dyn_random_sample
 * contains no user JS at all -- the handle's last use is the draw, which has
 * already finished once this runs (dyna-nat.h contract). Consumes idx. */
static JSValue dyn_random_sample_emit(JSContext *ctx, JSValueConst src,
                                      uint32_t *idx, uint64_t n, int is_ta)
{
    JSValue res;
    uint64_t i;

    if (is_ta) {
        /* same species: allocate through the array's constructor, then fill
         * element-wise (type-correct for every bpe incl. BigInt64) */
        JSValue ctor = JS_GetPropertyStr(ctx, src, "constructor");
        JSValue cargs[1];
        size_t bo, bl, bp;
        int res_ok;
        if (JS_IsException(ctor)) {
            free(idx);
            return JS_EXCEPTION;
        }
        cargs[0] = JS_NewInt64(ctx, (int64_t)n);
        res = JS_CallConstructor(ctx, ctor, 1, cargs);
        JS_FreeValue(ctx, cargs[0]);   /* constructor args are borrowed */
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(res)) {
            free(idx);
            return JS_EXCEPTION;
        }
        res_ok = 1;
        {
            JSValue rbuf = JS_GetTypedArrayBuffer(ctx, res, &bo, &bl, &bp);
            size_t count;
            if (JS_IsException(rbuf)) {
                res_ok = 0;
            } else {
                count = bl / bp;
                if (count != n)
                    res_ok = 0;
                JS_FreeValue(ctx, rbuf);
            }
        }
        if (!res_ok) {
            JS_FreeValue(ctx, res);
            free(idx);
            return JS_ThrowTypeError(ctx,
                "sample: the array's constructor must yield a typed array of length n");
        }
    } else {
        res = JS_NewArray(ctx);
        if (JS_IsException(res)) {
            free(idx);
            return JS_EXCEPTION;
        }
    }

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, src, idx[i]);
        if (JS_IsException(v) ||
            JS_SetPropertyUint32(ctx, res, (uint32_t)i, v) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, res);
            free(idx);
            return JS_EXCEPTION;
        }
    }
    free(idx);
    return res;
}

static JSValue dyn_random_sample(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val = JS_UNDEFINED;
    size_t byte_off = 0, byte_len = 0, bpe = 0;
    double len = 0.0, dn;
    uint64_t n;
    uint32_t *idx;
    int is_ta;

    (void)argc;
    /* Coerce-first (dyna-nat.h contract): argv[0]'s dims (the length getter)
     * and argv[1]'s ToFloat64 (valueOf) run user JS that may close() the
     * generator -- so every coercion happens BEFORE the raw handle is
     * resolved. What remains after the resolve is the draw (pure C) and the
     * emit helper, which runs only after the handle's last use. */
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        len = (double)(byte_len / bpe);
        /* the source view was only needed for its dims: reads below go
         * through JS_GetPropertyUint32, so release the buffer reference */
        JS_FreeValue(ctx, buf_val);
    } else if (dyn_random_array_len(ctx, argv[0], &len)) {
        return JS_EXCEPTION;
    }

    /* n required, integral, in [0, len]: sampling MORE than the population
     * is refused, not wrapped */
    if (argc < 2 || JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(ctx, "sample(arr, n): n is required");
    if (JS_ToFloat64(ctx, &dn, argv[1]))
        return JS_EXCEPTION;
    if (!(dn >= 0.0) || dn > len || dn != floor(dn))
        return JS_ThrowRangeError(ctx,
            "sample(arr, n): n must be an integer in [0, the array length]");
    n = (uint64_t)dn;

    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    idx = dyn_random_sample_indices(rng, (uint64_t)len, n);
    if (!idx)
        return JS_ThrowOutOfMemory(ctx);
    /* rng's last use was the draw: the tail runs user JS freely */
    return dyn_random_sample_emit(ctx, argv[0], idx, n, is_ta);
}

/* Element fetch for choice(): runs user JS (an element getter may close()
 * the generator). It lives in its own function so the raw-handle window in
 * dyn_random_choice contains no user JS at all: by the time this runs, the
 * handle's last use -- the draw -- is already behind the caller
 * (dyna-nat.h contract). */
static JSValue dyn_random_elem_at(JSContext *ctx, JSValueConst arr,
                                  uint32_t pick)
{
    return JS_GetPropertyUint32(ctx, arr, pick);
}

static JSValue dyn_random_choice(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_prng_t *rng;
    JSValue buf_val;
    size_t byte_off, byte_len, bpe;
    double len;
    uint64_t pick;
    int is_ta;

    (void)argc;
    /* Coerce-first (dyna-nat.h contract): argv[0]'s dims and its length
     * read run user JS (a length getter can close() the generator), so the
     * handle is resolved only once the shape is fully known. What remains
     * after the resolve is the draw (pure C) and the element fetch, which
     * runs after the handle's last use. */
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        JS_FreeValue(ctx, buf_val);
        len = (double)(byte_len / bpe);
    } else if (dyn_random_array_len(ctx, argv[0], &len)) {
        return JS_EXCEPTION;
    }
    if (len < 1.0)
        return JS_ThrowRangeError(ctx, "choice: the array is empty");
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    pick = dyn_prng_next_bounded(rng, (uint64_t)len);
    return dyn_random_elem_at(ctx, argv[0], (uint32_t)pick);
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
    JS_CFUNC_DEF("normal", 2, dyn_random_normal),
    JS_CFUNC_DEF("exponential", 1, dyn_random_exponential),
    JS_CFUNC_DEF("poisson", 1, dyn_random_poisson),
    JS_CFUNC_DEF("shuffle", 1, dyn_random_shuffle),
    JS_CFUNC_DEF("sample", 2, dyn_random_sample),
    JS_CFUNC_DEF("choice", 1, dyn_random_choice),
    JS_CFUNC_DEF("fill", 3, dyn_random_fill),
    JS_CFUNC_DEF("bytes", 1, dyn_random_bytes),
    JS_CFUNC_DEF("jump", 0, dyn_random_jump),
    JS_CFUNC_DEF("longJump", 0, dyn_random_long_jump),
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
