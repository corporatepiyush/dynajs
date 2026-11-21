#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

JSValue js_simd_axpy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *y, *x;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t ny, nx;
    double alpha;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > ny)
            return JS_ThrowRangeError(ctx, "axpy: window out of range");
        if (_woff + _wlen > nx)
            return JS_ThrowRangeError(ctx, "axpy: window out of range");
        y += _woff;
        ny = _wlen;
        x += _woff;
        nx = _wlen;
    }
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "axpy: length mismatch");
    simd.axpy(y, (float)alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32_w(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        out += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "add: length mismatch");
    simd.add(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32_w(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        out += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "mul: length mismatch");
    simd.mul(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_f64_axpy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *y, *x;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t ny, nx;
    double alpha;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64_w(ctx, argv[0], &y, &ny) || simd_get_f64(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > ny)
            return JS_ThrowRangeError(ctx, "f64Axpy: window out of range");
        if (_woff + _wlen > nx)
            return JS_ThrowRangeError(ctx, "f64Axpy: window out of range");
        y += _woff;
        ny = _wlen;
        x += _woff;
        nx = _wlen;
    }
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "f64Axpy: length mismatch");
    simd.f64_axpy(y, alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_i32_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t *o, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_i32_w(ctx, argv[0], &o, &no) || simd_get_i32(ctx, argv[1], &a, &na) || simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        o += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "i32Add: length mismatch");
    simd.i32_add(o, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

JSValue js_simd_i32_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t *o, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_i32_w(ctx, argv[0], &o, &no) || simd_get_i32(ctx, argv[1], &a, &na) || simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        o += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "i32Mul: length mismatch");
    simd.i32_mul(o, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

#endif
