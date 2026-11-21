#include "simd-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

static _Atomic JSClassID simd_cid_int32;

static _Atomic JSClassID simd_cid_float32;

static _Atomic JSClassID simd_cid_float64;

int simd_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 8) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Float64Array");
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
    *pp = (double*)(base + off);
    *pn = len / 8;
    return 0;
}

int simd_get_i32(JSContext* ctx, JSValueConst v, int32_t** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;
    JSClassID cid;

    JS_GetAnyOpaque(v, &cid);
    if (cid != (JSClassID)simd_cid_int32) {
        JS_ThrowTypeError(ctx, "expected an Int32Array");
        return -1;
    }
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
    *pp = (int32_t*)(base + off);
    *pn = len / 4;
    return 0;
}

static JSValue js_simd_norm_l1(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "normL1: window out of range");
        a += _woff;
        n = _wlen;
    }
    return JS_NewFloat64(ctx, simd_f32_norm_l1_wide(a, n));
}

static JSValue js_simd_norm_l2(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "normL2: window out of range");
        a += _woff;
        n = _wlen;
    }
    return JS_NewFloat64(ctx, (double)simd.norm_l2(a, n));
}

static JSValue js_simd_argmax(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "argmax: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmax: empty array");
    {
        size_t r;
        if (simd_f32_has_nan(a, n))
            return JS_NewInt64(ctx, (int64_t)simd_f32_arg_skip_nan(a, n, 1));
        r = simd.argmax(a, n);
        return JS_NewInt64(ctx, (int64_t)simd_f32_first_equal(a, n, a[r]));
    }
}

static JSValue js_simd_argmin(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "argmin: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmin: empty array");
    {
        size_t r;
        if (simd_f32_has_nan(a, n))
            return JS_NewInt64(ctx, (int64_t)simd_f32_arg_skip_nan(a, n, 0));
        r = simd.argmin(a, n);
        return JS_NewInt64(ctx, (int64_t)simd_f32_first_equal(a, n, a[r]));
    }
}

static JSValue js_simd_sub(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        out += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "sub: length mismatch");
    simd.sub(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_div(JSContext* ctx, JSValueConst this_val,
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
            return JS_ThrowRangeError(ctx, "div: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "div: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "div: window out of range");
        out += _woff;
        no = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "div: length mismatch");
    simd.div(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_abs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *out, *in;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, ni;
    (void)this_val;
    (void)argc;
    if (simd_get_f32_w(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &in, &ni))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "abs: window out of range");
        if (_woff + _wlen > ni)
            return JS_ThrowRangeError(ctx, "abs: window out of range");
        out += _woff;
        no = _wlen;
        in += _woff;
        ni = _wlen;
    }
    if (no != ni)
        return JS_ThrowRangeError(ctx, "abs: length mismatch");
    simd.abs(out, in, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_fma(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float *z, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t nz, na, nb;
    (void)this_val;
    (void)argc;
    if (simd_get_f32_w(ctx, argv[0], &z, &nz) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > nz)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        z += _woff;
        nz = _wlen;
        a += _woff;
        na = _wlen;
        b += _woff;
        nb = _wlen;
    }
    if (nz != na || na != nb)
        return JS_ThrowRangeError(ctx, "fma: length mismatch");
    simd.fma(z, a, b, nz);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_add_s(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "addScalar: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.add_s(a, a, (float)s, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_sigmoid(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "sigmoid: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.sigmoid(a, a, n);
    else
        simd.sigmoid(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "relu: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.relu(a, a, n);
    else
        simd.relu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu6(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "relu6: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.relu6(a, a, n);
    else
        simd.relu6(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_leaky_relu(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double slope;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &slope, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "leakyRelu: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.leaky_relu(a, a, (float)slope, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_elu(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double alpha;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "elu: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.elu(a, a, (float)alpha, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_tanh_fast(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "tanhFast: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.tanh_fast(a, a, n);
    else
        simd.tanh_fast(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_gelu(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "gelu: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.gelu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_silu(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "silu: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd_f32_unary_same_kernel(simd.silu, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_softmax(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "softmax: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "softmax: empty array");
    if (simd_f32_has_nan(a, n))
        simd_scalar.softmax(a, a, n);
    else
        simd.softmax(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_log_softmax(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "logSoftmax: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "logSoftmax: empty array");
    simd.log_softmax(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vexp(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vexp: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.vexp(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vlog(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vlog: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.vlog(a, a, n);
    else
        simd.vlog(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vsqrt(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vsqrt: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.vsqrt(a, a, n);
    else
        simd.vsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vrsqrt(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vrsqrt: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nonpositive(a, n))
        simd_scalar.vrsqrt(a, a, n);
    else
        simd.vrsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vinv(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vinv: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nonpositive(a, n))
        simd_scalar.vinv(a, a, n);
    else
        simd.vinv(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_gemm(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t m64, n64, k64;
    double alpha, beta;
    float *c, *a, *b;
    size_t nc, na, nb, m, n, k;
    (void)this_val;
    (void)argc;
    if (JS_ToIndex(ctx, &m64, argv[3]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &n64, argv[4]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &k64, argv[5]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &alpha, argv[6]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &beta, argv[7]))
        return JS_EXCEPTION;
    m = (size_t)m64;
    n = (size_t)n64;
    k = (size_t)k64;
    if (simd_dims_overflow(m, k) || simd_dims_overflow(k, n) || simd_dims_overflow(m, n))
        return JS_ThrowRangeError(ctx, "gemm: dimension overflow");
    if (simd_get_f32_w(ctx, argv[0], &c, &nc) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (nc != m * n)
        return JS_ThrowRangeError(ctx, "gemm: c.length must equal m*n");
    if (na != m * k)
        return JS_ThrowRangeError(ctx, "gemm: a.length must equal m*k");
    if (nb != k * n)
        return JS_ThrowRangeError(ctx, "gemm: b.length must equal k*n");
    if (simd_spans_overlap(c, nc * sizeof(float), a, na * sizeof(float))
        || simd_spans_overlap(c, nc * sizeof(float), b, nb * sizeof(float)))
        return JS_ThrowTypeError(ctx, "gemm: the output overlaps an input");
    simd.gemm(c, a, b, m, n, k, (float)alpha, (float)beta);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_clamp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double lo, hi;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "clamp: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.clamp(a, a, (float)lo, (float)hi, n);
    else
        simd.clamp(a, a, (float)lo, (float)hi, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_threshold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double t;
    (void)this_val;
    (void)argc;
    if (JS_ToFloat64(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "threshold: window out of range");
        a += _woff;
        n = _wlen;
    }
    simd.threshold(a, a, (float)t, n);
    return JS_DupValue(ctx, argv[0]);
}

static inline uint64_t simd_topk_key(float x, size_t i)
{
    uint32_t u;
    memcpy(&u, &x, sizeof u);
    if (x != x)
        u = 0xFFFFFFFFu;
    else {
        if (x == 0.0f)
            u = 0;
        u = (u & 0x80000000u) ? u : ~(u | 0x80000000u);
    }
    return (uint64_t)u << 32 | (uint32_t)i;
}

static void simd_topk_isort(uint64_t* a, size_t n)
{
    size_t i, j;
    for (i = 1; i < n; i++) {
        uint64_t v = a[i];
        for (j = i; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

static void simd_topk_nth(uint64_t* a, size_t n, size_t k)
{
    size_t lo = 0, hi = n, i, j;
    while (hi - lo > 16) {
        uint64_t x = a[lo], y = a[lo + (hi - lo) / 2], z = a[hi - 1], p, t;
        p = x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
        i = lo;
        j = hi - 1;
        for (;;) {
            while (a[i] < p)
                i++;
            while (a[j] > p)
                j--;
            if (i >= j)
                break;
            t = a[i];
            a[i] = a[j];
            a[j] = t;
            i++;
            j--;
        }
        if (k <= j + 1)
            hi = j + 1;
        else
            lo = j + 1;
        if (lo >= k || hi <= k)
            return;
    }
    simd_topk_isort(a + lo, hi - lo);
}

static size_t simd_topk_select(uint64_t* h, size_t k, size_t cap, const float* v, size_t n)
{
    size_t i, j, e, cnt = 0;
    float thr = NAN;
    int all = 1;
    for (i = 0; i < n;) {
        if (!all && thr == thr && i + 16 <= n) {
            uint32_t mask = 0;
            for (j = 0; j < 16; j++)
                mask |= (uint32_t)(v[i + j] > thr) << j;
            while (mask) {
                j = i + (size_t)__builtin_ctz(mask);
                mask &= mask - 1;
                h[cnt++] = simd_topk_key(v[j], j);
            }
            i += 16;
        } else {
            e = i + 16 < n ? i + 16 : n;
            for (; i < e; i++) {
                float x = v[i];
                if (all || (thr != thr ? x == x : x > thr))
                    h[cnt++] = simd_topk_key(x, i);
            }
        }
        if (cnt >= cap) {
            uint64_t m;
            simd_topk_nth(h, cnt, k);
            cnt = k;
            all = 0;
            m = h[0];
            for (j = 1; j < k; j++)
                m = h[j] > m ? h[j] : m;
            thr = v[(uint32_t)m];
        }
    }
    if (cnt > k) {
        simd_topk_nth(h, cnt, k);
        cnt = k;
    }
    return cnt;
}

static void simd_topk_sort(uint64_t* h, uint64_t* tmp, size_t k)
{
    uint32_t cnt[8][256];
    uint64_t *src = h, *dst = tmp, *t, all_or = 0, all_and = ~(uint64_t)0;
    size_t i, d;
    if (k <= 32) {
        simd_topk_isort(h, k);
        return;
    }
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < k; i++) {
        uint64_t x = h[i];
        all_or |= x;
        all_and &= x;
        for (d = 0; d < 8; d++)
            cnt[d][(x >> (8 * d)) & 255]++;
    }
    for (d = 0; d < 8; d++) {
        uint32_t s = 0, c;
        if ((((all_or ^ all_and) >> (8 * d)) & 255) == 0)
            continue;
        for (i = 0; i < 256; i++) {
            c = cnt[d][i];
            cnt[d][i] = s;
            s += c;
        }
        for (i = 0; i < k; i++)
            dst[cnt[d][(src[i] >> (8 * d)) & 255]++] = src[i];
        t = src;
        src = dst;
        dst = t;
    }
    if (src != h)
        memcpy(h, src, k * sizeof(uint64_t));
}

static JSValue js_simd_topk_indices(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t k64, stub[2], *keys;
    float* vals;
    size_t n, k, i, cap;
    uint32_t* idx;
    JSValue ab, out;
    JSValueConst ta_args[3];
    (void)this_val;
    (void)argc;
    if (JS_ToIndex(ctx, &k64, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &vals, &n))
        return JS_EXCEPTION;
    k = (size_t)k64;
    if (k > n)
        k = n;
    keys = stub;
    if (k) {
        cap = k < n - k ? 2 * k : n;
        keys = (uint64_t*)malloc(((cap > 2 * k ? cap : 2 * k) + 16) * sizeof(uint64_t));
        if (!keys)
            return JS_ThrowOutOfMemory(ctx);
        simd_topk_select(keys, k, cap, vals, n);
        simd_topk_sort(keys, keys + k, k);
    }
    idx = (uint32_t*)keys;
    for (i = 0; i < k; i++)
        idx[i] = (uint32_t)keys[i];
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)idx, k * sizeof(uint32_t));
    if (k)
        free(keys);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT32);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue js_simd_normalize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    float* a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double m, v, std;
    (void)this_val;
    (void)argc;
    if (simd_get_f32_w(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "normalize: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "normalize: empty array");
    m = simd_f32_sum_wide(a, n) / (double)n;
    simd.add_s(a, a, (float)-m, n);
    v = simd_f32_dot_wide(a, a, n) / (double)n;
    std = sqrt(v);
    simd.mul_s(a, a, (float)(1.0 / std), n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_cumsum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSClassID cid;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    (void)this_val;
    (void)argc;
    if (_whas < 0)
        return JS_EXCEPTION;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t* a;
        size_t n;
        if (simd_get_i32_w(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cumsum: window out of range");
            a += _woff;
            n = _wlen;
        }
        simd.i32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float* a;
        size_t n;
        if (simd_get_f32_w(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cumsum: window out of range");
            a += _woff;
            n = _wlen;
        }
        simd.f32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float64) {
        double* a;
        double acc = 0;
        size_t n, i;
        if (simd_get_f64_w(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cumsum: window out of range");
            a += _woff;
            n = _wlen;
        }
        for (i = 0; i < n; i++)
            a[i] = acc += a[i];
        return JS_DupValue(ctx, argv[0]);
    }
    return JS_ThrowTypeError(ctx, "cumsum: expected an Int32Array, Float32Array or Float64Array");
}

static JSValue js_simd_cummax(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSClassID cid;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    (void)this_val;
    (void)argc;
    if (_whas < 0)
        return JS_EXCEPTION;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t* a;
        size_t n;
        if (simd_get_i32_w(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cummax: window out of range");
            a += _woff;
            n = _wlen;
        }
        simd.i32_cummax(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float* a;
        size_t n;
        if (simd_get_f32_w(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cummax: window out of range");
            a += _woff;
            n = _wlen;
        }
        if (simd_f32_has_nan(a, n)) {
            size_t i = 0;
            float run;
            while (i < n && a[i] != a[i])
                i++;
            if (i < n) {
                run = a[i];
                for (; i < n; i++) {
                    if (a[i] > run)
                        run = a[i];
                    a[i] = run;
                }
            }
            return JS_DupValue(ctx, argv[0]);
        }
        simd.f32_cummax(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    return JS_ThrowTypeError(ctx, "cummax: expected an Int32Array or Float32Array");
}

static int simd_overlap(const void* a, size_t an, const void* b, size_t bn)
{
    const uint8_t *pa = (const uint8_t*)a, *pb = (const uint8_t*)b;
    return pa < pb + bn && pb < pa + an;
}

static int simd_check_indices(JSContext* ctx, const char* fn, const int32_t* idx,
    size_t n, size_t limit)
{
    uint32_t lim = limit < 0x80000000u ? (uint32_t)limit : 0x80000000u, bad = 0;
    size_t i = 0, j;
    for (; i + 64 <= n && !bad; i += 64)
        for (j = 0; j < 64; j++)
            bad |= (uint32_t)idx[i + j] >= lim;
    if (bad)
        i -= 64;
    for (; i < n; i++)
        if (idx[i] < 0 || (size_t)idx[i] >= limit) {
            JS_ThrowRangeError(ctx, "%s: idx[%zu] = %d is outside [0, %zu)", fn, i, idx[i], limit);
            return -1;
        }
    return 0;
}

static JSValue js_simd_gather(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* fn = magic ? "f64Gather" : "gather";
    int32_t* idx;
    size_t n_out, n_src, n_idx, i;
    void *out, *src;
    (void)this_val;
    (void)argc;
    if (magic) {
        if (simd_get_f64_w(ctx, argv[0], (double**)&out, &n_out) || simd_get_f64(ctx, argv[1], (double**)&src, &n_src))
            return JS_EXCEPTION;
    } else if (simd_get_f32_w(ctx, argv[0], (float**)&out, &n_out) || simd_get_f32(ctx, argv[1], (float**)&src, &n_src)) {
        return JS_EXCEPTION;
    }
    if (simd_get_i32(ctx, argv[2], &idx, &n_idx))
        return JS_EXCEPTION;
    if (n_idx != n_out)
        return JS_ThrowRangeError(ctx, "%s: out has %zu elements but idx has %zu", fn, n_out, n_idx);
    if (simd_overlap(out, n_out * (magic ? 8 : 4), src, n_src * (magic ? 8 : 4)) || simd_overlap(out, n_out * (magic ? 8 : 4), idx, n_idx * 4))
        return JS_ThrowRangeError(ctx, "%s: out must not share memory with src or idx", fn);
    if (simd_check_indices(ctx, fn, idx, n_idx, n_src))
        return JS_EXCEPTION;
    if (magic) {
        double *o = (double*)out, *sp = (double*)src;
        for (i = 0; i < n_idx; i++)
            o[i] = sp[idx[i]];
    } else {
        uint32_t *o = (uint32_t*)out, *sp = (uint32_t*)src;
        for (i = 0; i < n_idx; i++)
            o[i] = sp[idx[i]];
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_scatter_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* fn = magic ? "f64ScatterAdd" : "scatterAdd";
    int32_t* idx;
    size_t n_dst, n_val, n_idx, i;
    void *dst, *val;
    (void)this_val;
    (void)argc;
    if (magic) {
        if (simd_get_f64_w(ctx, argv[0], (double**)&dst, &n_dst))
            return JS_EXCEPTION;
    } else {
        if (JS_GetClassID(argv[0]) != (JSClassID)simd_cid_float32 || JS_GetClassID(argv[2]) != (JSClassID)simd_cid_float32)
            return JS_ThrowTypeError(ctx, "scatterAdd: dst and vals must be Float32Arrays (adding bit-cast integers is meaningless)");
        if (simd_get_f32_w(ctx, argv[0], (float**)&dst, &n_dst))
            return JS_EXCEPTION;
    }
    if (simd_get_i32(ctx, argv[1], &idx, &n_idx))
        return JS_EXCEPTION;
    if (magic ? simd_get_f64(ctx, argv[2], (double**)&val, &n_val) : simd_get_f32(ctx, argv[2], (float**)&val, &n_val))
        return JS_EXCEPTION;
    if (n_idx != n_val)
        return JS_ThrowRangeError(ctx, "%s: idx has %zu elements but vals has %zu", fn, n_idx, n_val);
    if (simd_overlap(dst, n_dst * (magic ? 8 : 4), val, n_val * (magic ? 8 : 4)) || simd_overlap(dst, n_dst * (magic ? 8 : 4), idx, n_idx * 4))
        return JS_ThrowRangeError(ctx, "%s: dst must not share memory with idx or vals", fn);
    if (simd_check_indices(ctx, fn, idx, n_idx, n_dst))
        return JS_EXCEPTION;
    if (magic) {
        double *d = (double*)dst, *v = (double*)val;
        for (i = 0; i < n_idx; i++)
            d[idx[i]] += v[i];
    } else {
        float *d = (float*)dst, *v = (float*)val;
        for (i = 0; i < n_idx; i++)
            d[idx[i]] += v[i];
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_gather_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* fn = magic ? "f64GatherAdd" : "gatherAdd";
    int32_t* idx;
    size_t n_dst, n_src, n_idx, w = magic ? 8 : 4;
    void *dst, *src;
    (void)this_val;
    (void)argc;
    if (magic) {
        if (simd_get_f64_w(ctx, argv[0], (double**)&dst, &n_dst) || simd_get_f64(ctx, argv[1], (double**)&src, &n_src))
            return JS_EXCEPTION;
    } else {
        if (JS_GetClassID(argv[0]) != (JSClassID)simd_cid_float32 || JS_GetClassID(argv[1]) != (JSClassID)simd_cid_float32)
            return JS_ThrowTypeError(ctx, "gatherAdd: dst and src must be Float32Arrays (adding bit-cast integers is meaningless)");
        if (simd_get_f32_w(ctx, argv[0], (float**)&dst, &n_dst) || simd_get_f32(ctx, argv[1], (float**)&src, &n_src))
            return JS_EXCEPTION;
    }
    if (simd_get_i32(ctx, argv[2], &idx, &n_idx))
        return JS_EXCEPTION;
    if (n_idx != n_dst)
        return JS_ThrowRangeError(ctx, "%s: dst has %zu elements but idx has %zu", fn, n_dst, n_idx);
    if (simd_overlap(dst, n_dst * w, src, n_src * w) || simd_overlap(dst, n_dst * w, idx, n_idx * 4))
        return JS_ThrowRangeError(ctx, "%s: dst must not share memory with src or idx", fn);
    if (simd_check_indices(ctx, fn, idx, n_idx, n_src))
        return JS_EXCEPTION;
    if (magic)
        simd_gather_add_f64((double*)dst, (const double*)src, idx, n_idx);
    else
        simd_gather_add_f32((float*)dst, (const float*)src, idx, n_idx);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_spmv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* fn = magic ? "f64Spmv" : "spmv";
    int32_t *col, *rp;
    size_t n_y, n_v, n_c, n_rp, n_x, r, w = magic ? 8 : 4;
    void *y, *val, *x;
    (void)this_val;
    (void)argc;
    if (!magic) {
        int j;
        for (j = 0; j < 5; j++)
            if (j != 2 && j != 3 && JS_GetClassID(argv[j]) != (JSClassID)simd_cid_float32)
                return JS_ThrowTypeError(ctx, "spmv: y, values and x must be Float32Arrays");
    }
    if (magic ? simd_get_f64_w(ctx, argv[0], (double**)&y, &n_y) : simd_get_f32_w(ctx, argv[0], (float**)&y, &n_y))
        return JS_EXCEPTION;
    if (magic ? simd_get_f64(ctx, argv[1], (double**)&val, &n_v) : simd_get_f32(ctx, argv[1], (float**)&val, &n_v))
        return JS_EXCEPTION;
    if (simd_get_i32(ctx, argv[2], &col, &n_c) || simd_get_i32(ctx, argv[3], &rp, &n_rp))
        return JS_EXCEPTION;
    if (magic ? simd_get_f64(ctx, argv[4], (double**)&x, &n_x) : simd_get_f32(ctx, argv[4], (float**)&x, &n_x))
        return JS_EXCEPTION;
    if (n_v != n_c)
        return JS_ThrowRangeError(ctx, "%s: values has %zu elements but columns has %zu", fn, n_v, n_c);
    if (n_rp != n_y + 1)
        return JS_ThrowRangeError(ctx, "%s: rowPointers has %zu elements, expected y.length + 1 = %zu", fn, n_rp, n_y + 1);
    if (rp[0] != 0)
        return JS_ThrowRangeError(ctx, "%s: rowPointers[0] = %d, expected 0", fn, rp[0]);
    for (r = 0; r < n_y; r++)
        if (rp[r + 1] < rp[r])
            return JS_ThrowRangeError(ctx, "%s: rowPointers decreases at %zu (%d < %d)", fn, r + 1, rp[r + 1], rp[r]);
    if ((size_t)rp[n_y] != n_v)
        return JS_ThrowRangeError(ctx, "%s: rowPointers[%zu] = %d but values has %zu elements", fn, n_y, rp[n_y], n_v);
    if (simd_overlap(y, n_y * w, val, n_v * w) || simd_overlap(y, n_y * w, x, n_x * w)
        || simd_overlap(y, n_y * w, col, n_c * 4) || simd_overlap(y, n_y * w, rp, n_rp * 4))
        return JS_ThrowRangeError(ctx, "%s: y must not share memory with values, columns, rowPointers or x", fn);
    if (simd_check_indices(ctx, fn, col, n_c, n_x))
        return JS_EXCEPTION;
    if (magic)
        simd_spmv_f64((double*)y, n_y, (const double*)val, col, rp, (const double*)x);
    else
        simd_spmv_f32((float*)y, n_y, (const float*)val, col, rp, (const float*)x);
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry js_simd_funcs[] = {
    JS_CFUNC_DEF("dot", 2, js_simd_dot),
    JS_CFUNC_DEF("sum", 1, js_simd_sum),
    JS_CFUNC_DEF("scale", 2, js_simd_scale),
    JS_CFUNC_DEF("axpy", 3, js_simd_axpy),
    JS_CFUNC_DEF("add", 3, js_simd_add),

    JS_CFUNC_DEF("normL1", 1, js_simd_norm_l1),
    JS_CFUNC_DEF("normL2", 1, js_simd_norm_l2),
    JS_CFUNC_DEF("max", 1, js_simd_max),
    JS_CFUNC_DEF("min", 1, js_simd_min),
    JS_CFUNC_DEF("argmax", 1, js_simd_argmax),
    JS_CFUNC_DEF("argmin", 1, js_simd_argmin),

    JS_CFUNC_DEF("sub", 3, js_simd_sub),
    JS_CFUNC_DEF("mul", 3, js_simd_mul),
    JS_CFUNC_DEF("div", 3, js_simd_div),
    JS_CFUNC_DEF("abs", 2, js_simd_abs),
    JS_CFUNC_DEF("fma", 3, js_simd_fma),

    JS_CFUNC_DEF("addScalar", 2, js_simd_add_s),
    JS_CFUNC_DEF("affine", 3, js_simd_scale_add_s),

    JS_CFUNC_DEF("sigmoid", 1, js_simd_sigmoid),
    JS_CFUNC_DEF("relu", 1, js_simd_relu),
    JS_CFUNC_DEF("relu6", 1, js_simd_relu6),
    JS_CFUNC_DEF("leakyRelu", 2, js_simd_leaky_relu),
    JS_CFUNC_DEF("elu", 2, js_simd_elu),
    JS_CFUNC_DEF("tanhFast", 1, js_simd_tanh_fast),
    JS_CFUNC_DEF("gelu", 1, js_simd_gelu),
    JS_CFUNC_DEF("silu", 1, js_simd_silu),

    JS_CFUNC_DEF("softmax", 1, js_simd_softmax),
    JS_CFUNC_DEF("logSoftmax", 1, js_simd_log_softmax),

    JS_CFUNC_DEF("vexp", 1, js_simd_vexp),
    JS_CFUNC_DEF("vlog", 1, js_simd_vlog),
    JS_CFUNC_DEF("vsqrt", 1, js_simd_vsqrt),
    JS_CFUNC_DEF("vrsqrt", 1, js_simd_vrsqrt),
    JS_CFUNC_DEF("vinv", 1, js_simd_vinv),

    JS_CFUNC_DEF("distL2", 2, js_simd_dist_l2),
    JS_CFUNC_DEF("distL1", 2, js_simd_dist_l1),
    JS_CFUNC_DEF("distCos", 2, js_simd_dist_cos),
    JS_CFUNC_DEF("distCheb", 2, js_simd_dist_cheb),

    JS_CFUNC_DEF("gemv", 6, js_simd_gemv),
    JS_CFUNC_DEF("gemvT", 6, js_simd_gemv_t),
    JS_CFUNC_DEF("gemm", 8, js_simd_gemm),

    JS_CFUNC_DEF("mean", 1, js_simd_mean),
    JS_CFUNC_DEF("variance", 1, js_simd_variance),
    JS_CFUNC_DEF("normalize", 1, js_simd_normalize),

    JS_CFUNC_DEF("clamp", 3, js_simd_clamp),
    JS_CFUNC_DEF("threshold", 2, js_simd_threshold),
    JS_CFUNC_DEF("topkIndices", 2, js_simd_topk_indices),

    JS_CFUNC_DEF("f64Sum", 1, js_simd_f64_sum),
    JS_CFUNC_DEF("f64Dot", 2, js_simd_f64_dot),
    JS_CFUNC_DEF("f64Max", 1, js_simd_f64_max),
    JS_CFUNC_DEF("f64Min", 1, js_simd_f64_min),
    JS_CFUNC_DEF("f64Scale", 2, js_simd_f64_scale),
    JS_CFUNC_DEF("f64Axpy", 3, js_simd_f64_axpy),
    JS_CFUNC_DEF("f64Mean", 1, js_simd_f64_mean),
    JS_CFUNC_DEF("f64Variance", 1, js_simd_f64_variance),

    JS_CFUNC_DEF("i32Sum", 1, js_simd_i32_sum),
    JS_CFUNC_DEF("i32Min", 1, js_simd_i32_min),
    JS_CFUNC_DEF("i32Max", 1, js_simd_i32_max),
    JS_CFUNC_DEF("i32Dot", 2, js_simd_i32_dot),
    JS_CFUNC_DEF("i32Add", 3, js_simd_i32_add),
    JS_CFUNC_DEF("i32Mul", 3, js_simd_i32_mul),
    JS_CFUNC_DEF("i32Scale", 2, js_simd_i32_scale),
    JS_CFUNC_DEF("i32Mean", 1, js_simd_i32_mean),

    JS_CFUNC_MAGIC_DEF("gather", 3, js_simd_gather, 0),
    JS_CFUNC_MAGIC_DEF("f64Gather", 3, js_simd_gather, 1),
    JS_CFUNC_MAGIC_DEF("scatterAdd", 3, js_simd_scatter_add, 0),
    JS_CFUNC_MAGIC_DEF("f64ScatterAdd", 3, js_simd_scatter_add, 1),
    JS_CFUNC_MAGIC_DEF("gatherAdd", 3, js_simd_gather_add, 0),
    JS_CFUNC_MAGIC_DEF("f64GatherAdd", 3, js_simd_gather_add, 1),
    JS_CFUNC_MAGIC_DEF("spmv", 5, js_simd_spmv, 0),
    JS_CFUNC_MAGIC_DEF("f64Spmv", 5, js_simd_spmv, 1),
    JS_CFUNC_DEF("cumsum", 1, js_simd_cumsum),
    JS_CFUNC_DEF("cummax", 1, js_simd_cummax),
};

static int js_simd_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

static JSClassID simd_capture_cid(JSContext* ctx, JSTypedArrayEnum type)
{
    JSValueConst args[1];
    JSValue ta;
    JSClassID cid = 0;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, type);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return cid;
}

int js_nat_init_simd(JSContext* ctx)
{
    JSModuleDef* m;
    simd_init();
    simd_cid_int32 = simd_capture_cid(ctx, JS_TYPED_ARRAY_INT32);
    simd_cid_float32 = simd_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT32);
    simd_cid_float64 = simd_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT64);
    m = JS_NewCModule(ctx, "dyna:simd", js_simd_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

#endif
