#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

JSValue js_simd_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "dot: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "dot: length mismatch");
    return JS_NewFloat64(ctx, simd_f32_dot_wide(a, b, na));
}

JSValue js_simd_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double k;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "scale: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.mul_s(a, a, (float)k, n);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_scale_add_s(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double alpha, beta;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &beta, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "affine: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.scale_add_s(a, (float)alpha, a, (float)beta, n);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_dist_l2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distL2: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distL2: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL2: length mismatch");
    return JS_NewFloat64(ctx, (double)simd.dist_l2(a, b, na));
}

JSValue js_simd_dist_l1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distL1: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distL1: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL1: length mismatch");
    return JS_NewFloat64(ctx, (double)simd.dist_l1(a, b, na));
}

JSValue js_simd_dist_cos(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distCos: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distCos: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCos: length mismatch");
    return JS_NewFloat64(ctx, (double)simd.dist_cos(a, b, na));
}

JSValue js_simd_dist_cheb(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distCheb: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distCheb: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCheb: length mismatch");
    {
        double r = (double)simd.dist_cheb(a, b, na);
        if (!isnan(r) && (simd_f32_has_nan(a, na) || simd_f32_has_nan(b, na)))
            r = DYN_NAN;
        return JS_NewFloat64(ctx, r);
    }
}

JSValue js_simd_gemv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t m64, n64;
    double alpha, beta;
    float *y, *a, *x, *tmp;
    size_t ny, na, nx, m, n, i;
    (void)this_val;
    if (JS_ToIndex(ctx, &m64, argv[3]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &n64, argv[4]))
        return JS_EXCEPTION;
    if (argc >= 7) {
        if (JS_ToFloat64(ctx, &alpha, argv[5]))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &beta, argv[6]))
            return JS_EXCEPTION;
    } else {
        alpha = 1.0;
        if (JS_ToFloat64(ctx, &beta, argv[5]))
            return JS_EXCEPTION;
    }
    m = (size_t)m64;
    n = (size_t)n64;
    if (simd_dims_overflow(m, n))
        return JS_ThrowRangeError(ctx, "gemv: m*n overflow");
    if (simd_get_f32_w(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != m)
        return JS_ThrowRangeError(ctx, "gemv: y.length must equal m");
    if (nx != n)
        return JS_ThrowRangeError(ctx, "gemv: x.length must equal n");
    if (na != m * n)
        return JS_ThrowRangeError(ctx, "gemv: a.length must equal m*n");
    if (simd_spans_overlap(y, ny * sizeof(float), a, na * sizeof(float))
        || simd_spans_overlap(y, ny * sizeof(float), x, nx * sizeof(float)))
        return JS_ThrowTypeError(ctx, "gemv: the output overlaps an input");
    if (argc >= 7) {
        tmp = (float*)malloc((m ? m : 1) * sizeof(float));
        if (!tmp)
            return JS_ThrowOutOfMemory(ctx);
        simd.gemv(tmp, a, x, m, n, 0.0f);
        if (beta == 0.0) {
            for (i = 0; i < m; i++)
                y[i] = (float)((double)alpha * (double)tmp[i]);
        } else {
            for (i = 0; i < m; i++)
                y[i] = (float)((double)alpha * (double)tmp[i] + (double)beta * (double)y[i]);
        }
        free(tmp);
    } else {
        simd.gemv(y, a, x, m, n, (float)beta);
    }
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_gemv_t(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t m64, n64;
    double beta;
    float *y, *a, *x;
    size_t ny, na, nx, m, n;
    (void)this_val;
    (void)argc;
    if (JS_ToIndex(ctx, &m64, argv[3]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &n64, argv[4]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &beta, argv[5]))
        return JS_EXCEPTION;
    m = (size_t)m64;
    n = (size_t)n64;
    if (simd_dims_overflow(m, n))
        return JS_ThrowRangeError(ctx, "gemvT: m*n overflow");
    if (simd_get_f32_w(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != n)
        return JS_ThrowRangeError(ctx, "gemvT: y.length must equal n");
    if (nx != m)
        return JS_ThrowRangeError(ctx, "gemvT: x.length must equal m");
    if (na != m * n)
        return JS_ThrowRangeError(ctx, "gemvT: a.length must equal m*n");
    if (simd_spans_overlap(y, ny * sizeof(float), a, na * sizeof(float))
        || simd_spans_overlap(y, ny * sizeof(float), x, nx * sizeof(float)))
        return JS_ThrowTypeError(ctx, "gemvT: the output overlaps an input");
    simd.gemv_t(y, a, x, m, n, (float)beta);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_f64_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &na) || simd_get_f64(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "f64Dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "f64Dot: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "f64Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.f64_dot(a, b, na));
}

JSValue js_simd_f64_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double s;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Scale: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.f64_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_i32_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &na) || simd_get_i32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Dot: window out of range");
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "i32Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.i32_dot(a, b, na));
}

JSValue js_simd_i32_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    int32_t s;
    (void)this_val;
    (void)argc;
    if (JS_ToInt32(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_i32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Scale: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.i32_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

#endif
