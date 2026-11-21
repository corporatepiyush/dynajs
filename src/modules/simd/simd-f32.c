#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

#define SIMD_TAIL_PAD 64
#define SIMD_F32_WIDE_LANES 8
#define SIMD_F32_WIDE_BIG_MIN 256

int simd_f32_has_nonpositive(const float* a, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!(a[i] > 0.0f))
            return 1;
    }
    return 0;
}

size_t simd_f32_first_equal(const float* a, size_t n, float v)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] == v)
            return i;
    }
    return 0;
}

int simd_get_f32(JSContext* ctx, JSValueConst v, float** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 4) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Float32Array");
        return -1;
    }
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
    *pp = (float*)(base + off);
    *pn = len / 4;
    return 0;
}

int simd_f32_has_nan(const float* a, size_t n)
{
    size_t i = 0, k, body = n & ~(size_t)(SIMD_NAN_SCAN_BLOCK - 1);
    for (; i < body; i += SIMD_NAN_SCAN_BLOCK) {
        unsigned bad = 0;
        for (k = 0; k < SIMD_NAN_SCAN_BLOCK; k++)
            bad |= (unsigned)(a[i + k] != a[i + k]);
        if (bad)
            return 1;
    }
    for (; i < n; i++)
        if (a[i] != a[i])
            return 1;
    return 0;
}

double simd_f32_dot_wide(const float* a, const float* b, size_t n)
{
    return n >= SIMD_F32_WIDE_BIG_MIN ? simd_f32_dot_lanes(a, b, n, SIMD_F32_WIDE_LANES_BIG)
                                      : simd_f32_dot_lanes(a, b, n, SIMD_F32_WIDE_LANES);
}

double simd_f32_centered_sq_wide(const float* a, float shift, size_t n)
{
    return n >= SIMD_F32_WIDE_BIG_MIN ? simd_f32_centered_sq_lanes(a, shift, n, SIMD_F32_WIDE_LANES_BIG)
                                      : simd_f32_centered_sq_lanes(a, shift, n, SIMD_F32_WIDE_LANES);
}

double simd_f32_sum_wide(const float* a, size_t n)
{
    return n >= SIMD_F32_WIDE_BIG_MIN ? simd_f32_sum_lanes(a, n, SIMD_F32_WIDE_LANES_BIG)
                                      : simd_f32_sum_lanes(a, n, SIMD_F32_WIDE_LANES);
}

double simd_f32_norm_l1_wide(const float* a, size_t n)
{
    return n >= SIMD_F32_WIDE_BIG_MIN ? simd_f32_norm_l1_lanes(a, n, SIMD_F32_WIDE_LANES_BIG)
                                      : simd_f32_norm_l1_lanes(a, n, SIMD_F32_WIDE_LANES);
}

void simd_f32_unary_same_kernel(void (*kernel)(float*, const float*, size_t),
    float* a, size_t n)
{
    size_t body = n & ~(size_t)(SIMD_TAIL_PAD - 1);
    size_t rem = n - body;
    if (body)
        kernel(a, a, body);
    if (rem) {
        float pad[SIMD_TAIL_PAD];
        memset(pad, 0, sizeof(pad));
        memcpy(pad, a + body, rem * sizeof(float));
        kernel(pad, pad, SIMD_TAIL_PAD);
        memcpy(a + body, pad, rem * sizeof(float));
    }
}

size_t simd_f32_arg_skip_nan(const float* a, size_t n, int want_max)
{
    size_t best = 0, i = 0;
    while (i < n && a[i] != a[i])
        i++;
    if (i == n)
        return 0;
    best = i;
    for (i++; i < n; i++) {
        if (a[i] != a[i])
            continue;
        if (want_max ? a[i] > a[best] : a[i] < a[best])
            best = i;
    }
    return best;
}

int simd_get_f32_w(JSContext* ctx, JSValueConst v, float** pp, size_t* pn)
{
    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    return simd_get_f32(ctx, v, pp, pn);
}

#endif
