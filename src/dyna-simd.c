#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#include "dyna-simd-kernels.h"

static _Atomic JSClassID simd_cid_int32;
static _Atomic JSClassID simd_cid_float32;

static int simd_f32_has_nonpositive(const float* a, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!(a[i] > 0.0f))
            return 1;
    }
    return 0;
}

static size_t simd_f32_first_equal(const float* a, size_t n, float v)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] == v)
            return i;
    }
    return 0;
}

static int simd_get_f32(JSContext* ctx, JSValueConst v, float** pp, size_t* pn)
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

static int simd_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn)
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

static int simd_get_i32(JSContext* ctx, JSValueConst v, int32_t** pp, size_t* pn)
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

static int simd_dims_overflow(size_t a, size_t b)
{
    return a != 0 && b > (size_t)-1 / a;
}

#define SIMD_NAN_SCAN_BLOCK 64

static int simd_f32_has_nan(const float* a, size_t n)
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

#define SIMD_TAIL_PAD 64
#define SIMD_F32_WIDE_LANES 8

static double simd_f32_dot_wide(const float* a, const float* b, size_t n)
{
    double acc[SIMD_F32_WIDE_LANES] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(SIMD_F32_WIDE_LANES - 1);
    double total = 0.0;
    for (; i < body; i += SIMD_F32_WIDE_LANES)
        for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
            acc[k] += (double)a[i + k] * (double)b[i + k];
    for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)a[i] * (double)b[i];
    return total;
}

static double simd_f32_centered_sq_wide(const float* a, float shift, size_t n)
{
    double acc[SIMD_F32_WIDE_LANES] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(SIMD_F32_WIDE_LANES - 1);
    double total = 0.0;
    for (; i < body; i += SIMD_F32_WIDE_LANES)
        for (k = 0; k < SIMD_F32_WIDE_LANES; k++) {
            float t = a[i + k] + shift;
            acc[k] += (double)t * (double)t;
        }
    for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
        total += acc[k];
    for (; i < n; i++) {
        float t = a[i] + shift;
        total += (double)t * (double)t;
    }
    return total;
}

static double simd_f32_sum_wide(const float* a, size_t n)
{
    double acc[SIMD_F32_WIDE_LANES] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(SIMD_F32_WIDE_LANES - 1);
    double total = 0.0;
    for (; i < body; i += SIMD_F32_WIDE_LANES)
        for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
            acc[k] += (double)a[i + k];
    for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)a[i];
    return total;
}

static double simd_f32_norm_l1_wide(const float* a, size_t n)
{
    double acc[SIMD_F32_WIDE_LANES] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(SIMD_F32_WIDE_LANES - 1);
    double total = 0.0;
    for (; i < body; i += SIMD_F32_WIDE_LANES)
        for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
            acc[k] += (double)fabsf(a[i + k]);
    for (k = 0; k < SIMD_F32_WIDE_LANES; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)fabsf(a[i]);
    return total;
}

static void simd_f32_unary_same_kernel(void (*kernel)(float*, const float*, size_t),
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

static size_t simd_f32_arg_skip_nan(const float* a, size_t n, int want_max)
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

static int simd_spans_overlap(const void* p, size_t pbytes, const void* q,
    size_t qbytes)
{
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)q;
    return pbytes && qbytes && a < b + qbytes && b < a + pbytes;
}

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

static int simd_window_arg(JSContext* ctx, int64_t* out, JSValueConst v)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (d != d)
        d = 0.0;
    if (d > 9007199254740991.0 || d < -9007199254740991.0) {
        JS_ThrowRangeError(ctx, "offset/length/limit out of range");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

static int simd_window(JSContext* ctx, int argc, JSValueConst* argv, int base,
    size_t* poff, size_t* plen)
{
    int64_t off = 0, len = -1;
    if (argc == base)
        return 0;
    if (argc == base + 1 && JS_IsObject(argv[base])) {
        JSValue o = argv[base];
        JSValue v;
        int has_len = 0;
        {
            JSPropertyEnum* props = NULL;
            uint32_t nprops = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
                return -1;
            for (i = 0; i < nprops; i++) {
                const char* nm = JS_AtomToCString(ctx, props[i].atom);
                int ok = 0;
                if (nm) {
                    ok = !strcmp(nm, "offset") || !strcmp(nm, "length") || !strcmp(nm, "limit");
                    if (!ok)
                        JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: "
                                               "offset, length, limit)",
                            nm);
                    JS_FreeCString(ctx, nm);
                }
                if (!ok) {
                    uint32_t j;
                    for (j = 0; j < nprops; j++)
                        JS_FreeAtom(ctx, props[j].atom);
                    js_free(ctx, props);
                    return -1;
                }
            }
            for (uint32_t j = 0; j < nprops; j++)
                JS_FreeAtom(ctx, props[j].atom);
            js_free(ctx, props);
        }
        v = JS_GetPropertyStr(ctx, o, "offset");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (simd_window_arg(ctx, &off, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "length");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (simd_window_arg(ctx, &len, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            has_len = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "limit");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t lim;
            if (simd_window_arg(ctx, &lim, v) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (has_len) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "length and limit are aliases (never both)");
                return -1;
            }
            len = lim;
            has_len = 1;
        }
        JS_FreeValue(ctx, v);
        if (!has_len) {
            JS_ThrowTypeError(ctx, "window requires length (or limit)");
            return -1;
        }
    } else if (argc == base + 2) {
        if (simd_window_arg(ctx, &off, argv[base]) < 0)
            return -1;
        if (simd_window_arg(ctx, &len, argv[base + 1]) < 0)
            return -1;
    } else {
        return (JS_ThrowTypeError(ctx, "expected %d args plus optional (offset, length)", base), -1);
    }
    if (off < 0 || len < 0) {
        JS_ThrowRangeError(ctx, "window offset/length must be >= 0");
        return -1;
    }
    *poff = (size_t)off;
    *plen = (size_t)len;
    return 1;
}

static JSValue js_simd_dot(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_sum(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_scale(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_axpy(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[2], &x, &nx))
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

static JSValue js_simd_add(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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

static JSValue js_simd_max(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_min(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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

static JSValue js_simd_mul(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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
    if (simd_get_f32(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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
    if (simd_get_f32(ctx, argv[0], &out, &no) || simd_get_f32(ctx, argv[1], &in, &ni))
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
    if (simd_get_f32(ctx, argv[0], &z, &nz) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_scale_add_s(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vexp: window out of range");
        a += _woff;
        n = _wlen;
    }
    if (simd_f32_has_nan(a, n))
        simd_scalar.vexp(a, a, n);
    else
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_dist_l2(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_dist_l1(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_dist_cos(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_dist_cheb(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_gemv(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &x, &nx))
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

static JSValue js_simd_gemv_t(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &x, &nx))
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
    if (simd_get_f32(ctx, argv[0], &c, &nc) || simd_get_f32(ctx, argv[1], &a, &na) || simd_get_f32(ctx, argv[2], &b, &nb))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_topk_indices(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t k64;
    float* vals;
    size_t n, k;
    uint32_t stub = 0, *idx;
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
    idx = &stub;
    if (k) {
        idx = (uint32_t*)malloc(k * sizeof(uint32_t));
        if (!idx)
            return JS_ThrowOutOfMemory(ctx);
    }
    simd.topk_indices(vals, idx, n, k);
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)idx, k * sizeof(uint32_t));
    if (k)
        free(idx);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT32);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue js_simd_f64_sum(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_f64_dot(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_f64_max(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_f64_min(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_f64_scale(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f64(ctx, argv[0], &a, &n))
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

static JSValue js_simd_f64_axpy(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f64(ctx, argv[0], &y, &ny) || simd_get_f64(ctx, argv[2], &x, &nx))
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

static JSValue js_simd_i32_sum(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_i32_min(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_i32_max(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_i32_dot(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_i32_add(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_i32(ctx, argv[0], &o, &no) || simd_get_i32(ctx, argv[1], &a, &na) || simd_get_i32(ctx, argv[2], &b, &nb))
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

static JSValue js_simd_i32_mul(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_i32(ctx, argv[0], &o, &no) || simd_get_i32(ctx, argv[1], &a, &na) || simd_get_i32(ctx, argv[2], &b, &nb))
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

static JSValue js_simd_i32_scale(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_i32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_mean(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_variance(JSContext* ctx, JSValueConst this_val,
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
    if (simd_get_f32(ctx, argv[0], &a, &n))
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

static JSValue js_simd_f64_mean(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_f64_variance(JSContext* ctx, JSValueConst this_val,
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

static JSValue js_simd_i32_mean(JSContext* ctx, JSValueConst this_val,
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
        if (simd_get_i32(ctx, argv[0], &a, &n))
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
        if (simd_get_f32(ctx, argv[0], &a, &n))
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
    return JS_ThrowTypeError(ctx, "cumsum: expected an Int32Array or Float32Array");
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
        if (simd_get_i32(ctx, argv[0], &a, &n))
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
        if (simd_get_f32(ctx, argv[0], &a, &n))
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
    m = JS_NewCModule(ctx, "dyna:simd", js_simd_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

#endif
