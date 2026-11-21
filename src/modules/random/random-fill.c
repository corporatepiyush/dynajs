#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

static inline void dyn_random_store(uint8_t* base, size_t i, int is64, double x)
{
    if (is64)
        ((double*)base)[i] = x;
    else
        ((float*)base)[i] = (float)x;
}

JSValue dyn_random_fill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue buf_val;
    uint8_t* base;
    size_t byte_off, byte_len, bpe, ab_size, count;
    double offset = 0.0, length;
    size_t fill_off, fill_len;

    (void)argc;
    if (JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
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
        return JS_ThrowTypeError(ctx, "typed array buffer is detached");
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
    return JS_DupValue(ctx, this_val);
}

static int dyn_random_float_view(JSContext* ctx, JSValueConst v,
    const char* what, uint8_t** pbase, size_t* pn, int* is64)
{
    JSClassID cid = JS_GetClassID(v);
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    if (!cid || (cid != dyn_random_cid_f32 && cid != dyn_random_cid_f64)) {
        JS_ThrowTypeError(ctx, "%s: buf must be a Float32Array or Float64Array", what);
        return -1;
    }
    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "typed array buffer is detached");
        return -1;
    }
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pbase = base + off;
    *pn = len / bpe;
    *is64 = bpe == 8;
    return 0;
}

static void dyn_random_fill_normal(dyn_prng_t* rng, uint8_t* base, size_t n,
    int is64, double mu, double sigma)
{
    size_t i = 0;
    while (i < n) {
        double u, v, q, f;
        if (sigma == 0.0) {
            dyn_random_store(base, i++, is64, mu);
            continue;
        }
        do {
            u = 2.0 * dyn_prng_next_double(rng) - 1.0;
            v = 2.0 * dyn_prng_next_double(rng) - 1.0;
            q = u * u + v * v;
        } while (q >= 1.0 || q == 0.0);
        f = sqrt(-2.0 * log(q) / q);
        dyn_random_store(base, i++, is64, mu + sigma * u * f);
        if (i < n)
            dyn_random_store(base, i++, is64, mu + sigma * v * f);
    }
}

static void dyn_random_fill_uniform(dyn_prng_t* rng, uint8_t* base, size_t n,
    int is64, double lo, double hi)
{
    double w = hi - lo;
    float hi_below = nextafterf((float)hi, (float)lo);
    size_t i;
    for (i = 0; i < n; i++) {
        double x = lo + w * dyn_prng_next_double(rng);
        float f;
        if (x >= hi)
            x = nextafter(hi, lo);
        if (is64) {
            ((double*)base)[i] = x;
            continue;
        }
        f = (float)x;
        ((float*)base)[i] = f >= (float)hi ? hi_below : f;
    }
}

JSValue dyn_random_fill_dist(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "fillNormal", "fillUniform", "fillExponential" };
    const char* what = names[magic];
    dyn_prng_t* rng;
    uint8_t* base;
    size_t n, i;
    int is64;
    double a = 0.0, b = 1.0;

    if (magic == DYN_FILL_EXPONENTIAL) {
        if (dyn_random_arg_real(ctx, argv[1], argc > 1, 1.0, &a, "fillExponential(buf, lambda): lambda"))
            return JS_EXCEPTION;
        if (!(a > 0.0))
            return JS_ThrowRangeError(ctx, "fillExponential(buf, lambda): lambda must be a finite number > 0");
    } else {
        if (dyn_random_arg_real(ctx, argv[1], argc > 1, 0.0, &a, magic == DYN_FILL_NORMAL ? "fillNormal(buf, mu, sigma): mu" : "fillUniform(buf, lo, hi): lo")
            || dyn_random_arg_real(ctx, argv[2], argc > 2, 1.0, &b, magic == DYN_FILL_NORMAL ? "fillNormal(buf, mu, sigma): sigma" : "fillUniform(buf, lo, hi): hi"))
            return JS_EXCEPTION;
        if (magic == DYN_FILL_NORMAL && b < 0.0)
            return JS_ThrowRangeError(ctx, "fillNormal(buf, mu, sigma): sigma must be >= 0");
        if (magic == DYN_FILL_UNIFORM && !(b > a && isfinite(b - a)))
            return JS_ThrowRangeError(ctx, "fillUniform(buf, lo, hi): hi must be greater than lo and hi - lo finite");
    }
    if (dyn_random_float_view(ctx, argv[0], what, &base, &n, &is64))
        return JS_EXCEPTION;
    rng = dyn_plain_get(ctx, this_val, dyn_random_class_id);
    if (!rng)
        return JS_EXCEPTION;
    if (magic == DYN_FILL_NORMAL)
        dyn_random_fill_normal(rng, base, n, is64, a, b);
    else if (magic == DYN_FILL_UNIFORM)
        dyn_random_fill_uniform(rng, base, n, is64, a, b);
    else
        for (i = 0; i < n; i++)
            dyn_random_store(base, i, is64, -log1p(-dyn_prng_next_double(rng)) / a);
    return JS_DupValue(ctx, this_val);
}

#endif
