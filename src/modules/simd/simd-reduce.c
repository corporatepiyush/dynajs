#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

static int simd_f64_has_nan(const double* a, size_t n)
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

JSValue js_simd_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "sum: window out of range");
        a += _woff;
        n = _wlen;
    }
    return JS_NewFloat64(ctx, simd_f32_sum_wide(a, n));
}

JSValue js_simd_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "max: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "max: empty array");
    {
        double m = (double)simd.max(a, n);
        if (!isnan(m) && simd_f32_has_nan(a, n))
            m = DYN_NAN;
        return JS_NewFloat64(ctx, m);
    }
}

JSValue js_simd_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "min: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "min: empty array");
    {
        double m = (double)simd.min(a, n);
        if (!isnan(m) && simd_f32_has_nan(a, n))
            m = DYN_NAN;
        return JS_NewFloat64(ctx, m);
    }
}

JSValue js_simd_f64_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Sum: window out of range");
        a += _woff;
        n = _wlen;
    }
    return JS_NewFloat64(ctx, simd.f64_sum(a, n));
}

JSValue js_simd_f64_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Max: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Max: empty array");
    {
        double m = simd.f64_max(a, n);
        if (!isnan(m) && simd_f64_has_nan(a, n))
            m = DYN_NAN;
        return JS_NewFloat64(ctx, m);
    }
}

JSValue js_simd_f64_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Min: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Min: empty array");
    {
        double m = simd.f64_min(a, n);
        if (!isnan(m) && simd_f64_has_nan(a, n))
            m = DYN_NAN;
        return JS_NewFloat64(ctx, m);
    }
}

JSValue js_simd_i32_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Sum: window out of range");
        a += _woff;
        n = _wlen;
    }
    return JS_NewInt64(ctx, simd.i32_sum(a, n));
}

JSValue js_simd_i32_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Min: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Min: empty array");
    return JS_NewInt32(ctx, simd.i32_min(a, n));
}

JSValue js_simd_i32_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Max: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Max: empty array");
    return JS_NewInt32(ctx, simd.i32_max(a, n));
}

JSValue js_simd_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "mean: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "mean: empty array");
    return JS_NewFloat64(ctx, simd_f32_sum_wide(a, n) / (double)n);
}

JSValue js_simd_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double m, v;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "variance: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "variance: empty array");
    m = simd_f32_sum_wide(a, n) / (double)n;
    v = simd_f32_centered_sq_wide(a, (float)-m, n) / (double)n;
    return JS_NewFloat64(ctx, v);
}

JSValue js_simd_f64_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Mean: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Mean: empty array");
    return JS_NewFloat64(ctx, simd.f64_sum(a, n) / (double)n);
}

JSValue js_simd_f64_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *a, *tmp;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n, i;
    double m, v;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Variance: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Variance: empty array");
    m = simd.f64_sum(a, n) / (double)n;
    tmp = (double*)malloc(n * sizeof(double));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++)
        tmp[i] = a[i] - m;
    v = simd.f64_dot(tmp, tmp, n) / (double)n;
    free(tmp);
    return JS_NewFloat64(ctx, v);
}

JSValue js_simd_i32_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val;
    (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Mean: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Mean: empty array");
    return JS_NewFloat64(ctx, (double)simd.i32_sum(a, n) / (double)n);
}

#endif
