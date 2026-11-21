#include "random-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

typedef struct {
    uint32_t* head;
    uint64_t* keys;
    uint32_t* vals;
    size_t mask;
    uint64_t n;
} dyn_random_sample_t;

static void dyn_random_sample_free(dyn_random_sample_t* s)
{
    dyn_nat_free(s->head);
    dyn_nat_free(s->keys);
    dyn_nat_free(s->vals);
    s->head = NULL;
    s->keys = NULL;
    s->vals = NULL;
}

static size_t dyn_random_sample_slot(const dyn_random_sample_t* s, uint64_t k)
{
    uint64_t h = k * 0x9e3779b97f4a7c15ULL;
    size_t i = (size_t)(h >> 32) & s->mask;
    while (s->keys[i] != UINT64_MAX && s->keys[i] != k)
        i = (i + 1) & s->mask;
    return i;
}

static int dyn_random_sample_put(dyn_random_sample_t* s, uint64_t k,
    uint32_t v)
{
    size_t i;

    if (!s->keys) {
        size_t cap = 8;
        if (s->n > ((size_t)-1) / 4)
            return -1;
        while (cap < (size_t)s->n * 2 + 2)
            cap *= 2;
        s->keys = (uint64_t*)dyn_nat_malloc(cap * sizeof(uint64_t));
        if (!s->keys)
            return -1;
        for (i = 0; i < cap; i++)
            s->keys[i] = UINT64_MAX;
        s->vals = (uint32_t*)dyn_nat_malloc(cap * sizeof(uint32_t));
        if (!s->vals) {
            dyn_nat_free(s->keys);
            s->keys = NULL;
            return -1;
        }
        s->mask = cap - 1;
    }
    i = dyn_random_sample_slot(s, k);
    s->keys[i] = k;
    s->vals[i] = v;
    return 0;
}

static uint32_t dyn_random_sample_get(const dyn_random_sample_t* s, uint64_t k)
{
    size_t i;

    if (k < s->n)
        return s->head[k];
    if (!s->keys)
        return (uint32_t)k;
    i = dyn_random_sample_slot(s, k);
    return s->keys[i] == UINT64_MAX ? (uint32_t)k : s->vals[i];
}

static int dyn_random_sample_set(dyn_random_sample_t* s, uint64_t k,
    uint32_t v)
{
    if (k < s->n) {
        s->head[k] = v;
        return 0;
    }
    return dyn_random_sample_put(s, k, v);
}

static int dyn_random_sample_indices(dyn_prng_t* rng, uint64_t len,
    uint64_t n, dyn_random_sample_t* s)
{
    uint64_t i;

    s->n = n;
    if (n == 0)
        return 0;
    if (n > ((size_t)-1) / sizeof(uint32_t))
        return -1;
    s->head = (uint32_t*)dyn_nat_malloc((size_t)n * sizeof(uint32_t));
    if (!s->head)
        return -1;
    for (i = 0; i < n; i++)
        s->head[i] = (uint32_t)i;
    for (i = 0; i < n; i++) {
        uint64_t j = i + dyn_prng_next_bounded(rng, len - i);
        uint32_t a = dyn_random_sample_get(s, i);
        uint32_t b = dyn_random_sample_get(s, j);
        if (dyn_random_sample_set(s, i, b) || dyn_random_sample_set(s, j, a))
            return -1;
    }
    return 0;
}

static JSValue dyn_random_sample_emit(JSContext* ctx, JSValueConst src,
    const dyn_random_sample_t* s, int is_ta)
{
    JSValue res;
    uint64_t i, n = s->n;

    if (is_ta) {
        JSValue ctor = JS_GetPropertyStr(ctx, src, "constructor");
        JSValue cargs[1];
        size_t bo, bl, bp;
        int res_ok;
        if (JS_IsException(ctor))
            return JS_EXCEPTION;
        cargs[0] = JS_NewInt64(ctx, (int64_t)n);
        res = JS_CallConstructor(ctx, ctor, 1, cargs);
        JS_FreeValue(ctx, cargs[0]);
        JS_FreeValue(ctx, ctor);
        if (JS_IsException(res))
            return JS_EXCEPTION;
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
            return JS_ThrowTypeError(ctx,
                "sample: the array's constructor must yield a typed array of length n");
        }
    } else {
        JSValue* vals;
        uint32_t count;
        if (n > 0 && n <= INT32_MAX && JS_GetFastArray(ctx, src, &vals, &count)) {
            JSValue* out = (JSValue*)dyn_nat_malloc((size_t)n * sizeof(JSValue));
            if (!out)
                return JS_ThrowOutOfMemory(ctx);
            for (i = 0; i < n; i++) {
                uint32_t k = dyn_random_sample_get(s, i);
                if (k >= count)
                    break;
                out[i] = vals[k];
            }
            if (i == n) {
                for (i = 0; i < n; i++)
                    JS_DupValue(ctx, out[i]);
                res = JS_NewArrayFrom(ctx, (int)n, out);
                dyn_nat_free(out);
                return res;
            }
            dyn_nat_free(out);
        }
        res = JS_NewArray(ctx);
        if (JS_IsException(res))
            return JS_EXCEPTION;
    }

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, src, dyn_random_sample_get(s, i));
        if (JS_IsException(v) || JS_DefinePropertyValueUint32(ctx, res, (uint32_t)i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}

JSValue dyn_random_sample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_prng_t* rng;
    JSValue buf_val = JS_UNDEFINED;
    size_t byte_off = 0, byte_len = 0, bpe = 0;
    double len = 0.0, dn;
    uint64_t n;
    dyn_random_sample_t s = { 0 };
    JSValue r;
    int is_ta;

    (void)argc;
    is_ta = dyn_random_view_ta(ctx, argv[0], &buf_val, &byte_off, &byte_len, &bpe);
    if (is_ta < 0)
        return JS_EXCEPTION;
    if (is_ta) {
        len = (double)(byte_len / bpe);
        JS_FreeValue(ctx, buf_val);
    } else if (dyn_random_array_len(ctx, argv[0], &len)) {
        return JS_EXCEPTION;
    }

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
    if (dyn_random_sample_indices(rng, (uint64_t)len, n, &s)) {
        dyn_random_sample_free(&s);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = dyn_random_sample_emit(ctx, argv[0], &s, is_ta);
    dyn_random_sample_free(&s);
    return r;
}

#endif
