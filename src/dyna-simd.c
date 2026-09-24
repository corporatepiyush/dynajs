/*
 * dyna:simd -- direct access to the shared SIMD kernel table.
 *
 * Dispatch is LAYERED: simd_override_* fills only non-NULL slots, applied
 * scalar -> SSE4.2 -> NEON -> SVE -> AVX2 -> AVX-512 (SVE applies before
 * the x86 tiers; the sets are host-disjoint), so a higher ISA
 * inheriting a kernel from a lower one is by design, not a gap.
 * For f64 reductions inside your own TU, portable multi-accumulator C beats
 * these calls -- they cannot inline. Use them for bulk work, not small loops.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Kernels come from the shared multi-ISA dispatch table (`simd`, installed once
 * by simd_init() at runtime startup): NEON/AVX2/AVX-512/SVE where available,
 * scalar otherwise. This is the same facility the engine core uses. */
#include "dyna-simd-kernels.h"

/* Built-in typed-array class ids for Int32Array / Float32Array, captured once
 * from sample instances at module init (built-in ids are process-global
 * constants). Used to distinguish the two 4-byte element types for cumsum/cummax
 * and to strictly type the i32* methods. _Atomic because a Worker's runtime
 * inits this module on its own thread (all inits store the same constant). */
static _Atomic JSClassID simd_cid_int32;
static _Atomic JSClassID simd_cid_float32;

/* ---------- JS <-> float32 backing buffer at the boundary ---------- */

/* Resolve a Float32Array argument to its backing float* and element count.
 * Returns 0 on success. Throws (and returns -1) for a non-typed-array, a
 * non-4-byte element type, or a detached buffer. The pointer is valid only for
 * the synchronous duration of the call (no JS runs before the kernel). */
static int simd_get_f32(JSContext *ctx, JSValueConst v, float **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
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
    if (!base) /* detached */
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (float *)(base + off);
    *pn = len / 4;
    return 0;
}

/* Resolve a Float64Array argument to its backing double* and element count.
 * Same contract as simd_get_f32 but for 8-byte elements (JS Number IS f64).
 * Throws (and returns -1) for a non-typed-array, a non-8-byte element type, or
 * a detached buffer. */
static int simd_get_f64(JSContext *ctx, JSValueConst v, double **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
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
    if (!base) /* detached */
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (double *)(base + off);
    *pn = len / 8;
    return 0;
}

/* Resolve an Int32Array to its backing int32* and element count. STRICT: rejects
 * any typed array that is not an Int32Array (a Float32Array/Uint32Array shares
 * the 4-byte stride but a different element meaning — never reinterpret its
 * bits). The class-id read runs no user JS, so it is safe before/after resolve. */
static int simd_get_i32(JSContext *ctx, JSValueConst v, int32_t **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
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
    if (!base) /* detached */
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (int32_t *)(base + off);
    *pn = len / 4;
    return 0;
}

/* True if a*b would overflow size_t (guards gemv/gemvT/gemm dimension
 * products against attacker-controlled m/n/k before they size a bounds
 * check or a buffer). */
static int simd_dims_overflow(size_t a, size_t b)
{
    return a != 0 && b > (size_t)-1 / a;
}

/* (offset,length) windows: channel slicing without subarray() copies.
 * Every vector kernel (all except gemv/gemvT/gemm/topkIndices, whose range is
 * owned by dims/k) accepts an optional trailing window: either two numbers
 * (offset, length) or one options bag {offset, length|limit}. The window is
 * uniform: the same offset/length applies to EVERY array arg, validated
 * against each length (offset+length must fit). Absent means the whole
 * array. The bag form rejects unknown keys, naming the key and the valid
 * set; limit is an accepted alias for length (never both). */
static int simd_window(JSContext *ctx, int argc, JSValueConst *argv, int base,
                       size_t *poff, size_t *plen)
{
    int64_t off = 0, len = -1;
    if (argc == base)
        return 0;
    if (argc == base + 1 && JS_IsObject(argv[base])) {
        JSValue o = argv[base];
        JSValue v;
        int has_len = 0;
        /* strict bag: offset, length, limit only */
        {
            JSPropertyEnum *props = NULL;
            uint32_t nprops = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
                return -1;
            for (i = 0; i < nprops; i++) {
                const char *nm = JS_AtomToCString(ctx, props[i].atom);
                int ok = 0;
                if (nm) {
                    ok = !strcmp(nm, "offset") || !strcmp(nm, "length") ||
                         !strcmp(nm, "limit");
                    if (!ok)
                        JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: "
                                          "offset, length, limit)", nm);
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
            if (JS_ToInt64(ctx, &off, v) < 0) { JS_FreeValue(ctx, v); return -1; }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "length");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &len, v) < 0) { JS_FreeValue(ctx, v); return -1; }
            has_len = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "limit");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int64_t lim;
            if (JS_ToInt64(ctx, &lim, v) < 0) { JS_FreeValue(ctx, v); return -1; }
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
        if (JS_ToInt64(ctx, &off, argv[base]) < 0)
            return -1;
        if (JS_ToInt64(ctx, &len, argv[base + 1]) < 0)
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


/* NOTE on small-n reduction safety: the max/min/argmax/argmin kernels in every
 * ISA table (dyna-simd-{neon,sse42,avx2,avx512,sve}.c) now guard their seed
 * load ("only seed a W-wide vector when n >= W", scalar path otherwise), so
 * every n >= 1 is safe to route straight into the table. An earlier binding
 * detoured 0 < n < 64 through a local scalar reduction to dodge a then-live
 * heap-overread in those kernels; that detour sent every small reduction (the
 * exact case SIMD exists for) through a scalar loop and is gone now. ASAN with
 * sizes 1..63 across the ISA kernels is the regression guard. */

static JSValue js_simd_dot(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "dot: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "dot: length mismatch");
    return JS_NewFloat64(ctx, simd.dot(a, b, na));
}

static JSValue js_simd_sum(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "sum: window out of range");
        a += _woff; n = _wlen;
    }
    return JS_NewFloat64(ctx, simd.sum(a, n));
}

static JSValue js_simd_scale(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double k;
    (void)this_val; (void)argc;
    /* coerce the scalar to a C local FIRST (may run user valueOf), then resolve
     * the buffer -- no JS between resolve and the kernel. */
    if (JS_ToFloat64(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "scale: window out of range");
        a += _woff; n = _wlen;
    }
    simd.mul_s(a, a, (float)k, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_axpy(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *y, *x;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t ny, nx;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > ny)
            return JS_ThrowRangeError(ctx, "axpy: window out of range");
        if (_woff + _wlen > nx)
            return JS_ThrowRangeError(ctx, "axpy: window out of range");
        y += _woff; ny = _wlen;
        x += _woff; nx = _wlen;
    }
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "axpy: length mismatch");
    simd.axpy(y, (float)alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_add(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "add: window out of range");
        out += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "add: length mismatch");
    simd.add(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- reductions ---------- */

static JSValue js_simd_norm_l1(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "normL1: window out of range");
        a += _woff; n = _wlen;
    }
    return JS_NewFloat64(ctx, simd.norm_l1(a, n));
}

static JSValue js_simd_norm_l2(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "normL2: window out of range");
        a += _woff; n = _wlen;
    }
    return JS_NewFloat64(ctx, simd.norm_l2(a, n));
}

static JSValue js_simd_max(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "max: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "max: empty array");
    return JS_NewFloat64(ctx, simd.max(a, n));
}

static JSValue js_simd_min(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "min: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "min: empty array");
    return JS_NewFloat64(ctx, simd.min(a, n));
}

static JSValue js_simd_argmax(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "argmax: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmax: empty array");
    return JS_NewInt64(ctx, (int64_t)simd.argmax(a, n));
}

static JSValue js_simd_argmin(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "argmin: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmin: empty array");
    return JS_NewInt64(ctx, (int64_t)simd.argmin(a, n));
}

/* ---------- element-wise: vector-vector -> vector (out separate, like add) ---------- */

static JSValue js_simd_sub(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "sub: window out of range");
        out += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "sub: length mismatch");
    simd.sub(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_mul(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "mul: window out of range");
        out += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "mul: length mismatch");
    simd.mul(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_div(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "div: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "div: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "div: window out of range");
        out += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "div: length mismatch");
    simd.div(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_abs(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *in;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, ni;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &in, &ni))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "abs: window out of range");
        if (_woff + _wlen > ni)
            return JS_ThrowRangeError(ctx, "abs: window out of range");
        out += _woff; no = _wlen;
        in += _woff; ni = _wlen;
    }
    if (no != ni)
        return JS_ThrowRangeError(ctx, "abs: length mismatch");
    simd.abs(out, in, no);
    return JS_DupValue(ctx, argv[0]);
}

/* fma(z, a, b): z += a*b, in-place accumulate (like axpy). */
static JSValue js_simd_fma(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *z, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t nz, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &z, &nz) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > nz)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "fma: window out of range");
        z += _woff; nz = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (nz != na || na != nb)
        return JS_ThrowRangeError(ctx, "fma: length mismatch");
    simd.fma(z, a, b, nz);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- scalar-vector: in-place on the single array (like scale) ---------- */

static JSValue js_simd_add_s(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double s;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "addScalar: window out of range");
        a += _woff; n = _wlen;
    }
    simd.add_s(a, a, (float)s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* affine(a, alpha, beta): a = alpha*a + beta, in-place. */
static JSValue js_simd_scale_add_s(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double alpha, beta;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &beta, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "affine: window out of range");
        a += _woff; n = _wlen;
    }
    simd.scale_add_s(a, (float)alpha, a, (float)beta, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- activations: in-place, one array in/out (header-documented safe) ---------- */

static JSValue js_simd_sigmoid(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "sigmoid: window out of range");
        a += _woff; n = _wlen;
    }
    simd.sigmoid(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "relu: window out of range");
        a += _woff; n = _wlen;
    }
    simd.relu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu6(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "relu6: window out of range");
        a += _woff; n = _wlen;
    }
    simd.relu6(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_leaky_relu(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double slope;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &slope, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "leakyRelu: window out of range");
        a += _woff; n = _wlen;
    }
    simd.leaky_relu(a, a, (float)slope, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_elu(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "elu: window out of range");
        a += _woff; n = _wlen;
    }
    simd.elu(a, a, (float)alpha, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_tanh_fast(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "tanhFast: window out of range");
        a += _woff; n = _wlen;
    }
    simd.tanh_fast(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_gelu(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "gelu: window out of range");
        a += _woff; n = _wlen;
    }
    simd.gelu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* silu(x) = x * sigmoid(x), computed by the simd.silu kernel slot. The x86
 * sign bug the binding once worked around by composing sigmoid+mul is fixed
 * at source (the Schraudolph magic constant is positive against neg_x in the
 * AVX2/SSE4.2/AVX-512/NEON kernels); the kernel is the same fast_exp class as
 * sigmoid and avoids the temp buffer + extra pass. It runs IN PLACE and is a
 * DIFFERENT formulation from the sigmoid+mul compose (measured ~5% apart at
 * x=-2.5, each inside the 2e-2 class bound) -- test_simd.js pins kernel vs
 * compose within that class, and in-place == fresh bitwise. */
static JSValue js_simd_silu(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "silu: window out of range");
        a += _woff; n = _wlen;
    }
    simd.silu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- softmax family: in-place; empty input would OOB-read in[0] in the
 * kernel, so reject it here rather than passing it through. ---------- */

static JSValue js_simd_softmax(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "softmax: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "softmax: empty array");
    simd.softmax(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_log_softmax(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "logSoftmax: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "logSoftmax: empty array");
    simd.log_softmax(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- unary math: in-place ---------- */

static JSValue js_simd_vexp(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vexp: window out of range");
        a += _woff; n = _wlen;
    }
    simd.vexp(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vlog(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vlog: window out of range");
        a += _woff; n = _wlen;
    }
    simd.vlog(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vsqrt(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vsqrt: window out of range");
        a += _woff; n = _wlen;
    }
    simd.vsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vrsqrt(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vrsqrt: window out of range");
        a += _woff; n = _wlen;
    }
    simd.vrsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vinv(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "vinv: window out of range");
        a += _woff; n = _wlen;
    }
    simd.vinv(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- distances: vector-vector -> scalar ---------- */

static JSValue js_simd_dist_l2(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distL2: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distL2: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL2: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_l2(a, b, na));
}

static JSValue js_simd_dist_l1(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distL1: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distL1: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL1: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_l1(a, b, na));
}

static JSValue js_simd_dist_cos(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distCos: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distCos: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCos: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_cos(a, b, na));
}

static JSValue js_simd_dist_cheb(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "distCheb: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "distCheb: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCheb: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_cheb(a, b, na));
}

/* ---------- BLAS-2/3: row-major, explicit dims, in-place on the output arg.
 * Dims/scalars are ALL coerced first (JS_ToIndex/JS_ToFloat64), before any
 * buffer is resolved -- same reentrancy rule as scale/axpy/add. ---------- */

/* gemv dispatches on the argument count:
 *   gemv(y, a, x, m, n, beta)          y = beta*y + A*x        (legacy form)
 *   gemv(y, a, x, m, n, alpha, beta)   y = alpha*A*x + beta*y   (gemm parity)
 * A is m x n, x is length n, y is length m. The declared length stays 6 so
 * Function.length is unchanged for existing callers. The legacy form runs the
 * gemv kernel directly; the alpha form composes it (beta=0 gives tmp = A*x
 * without reading y -- pinned NaN-poison behavior) with a scalar m-element
 * combine, because the kernel slot has no alpha: the m*n reduction stays
 * table-dispatched, only the m-wide combine is scalar. beta == 0 skips the y
 * read exactly like gemm's beta == 0 skips C. */
static JSValue js_simd_gemv(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
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
    if (simd_get_f32(ctx, argv[0], &y, &ny) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != m)
        return JS_ThrowRangeError(ctx, "gemv: y.length must equal m");
    if (nx != n)
        return JS_ThrowRangeError(ctx, "gemv: x.length must equal n");
    if (na != m * n)
        return JS_ThrowRangeError(ctx, "gemv: a.length must equal m*n");
    if (argc >= 7) {
        tmp = (float *)malloc((m ? m : 1) * sizeof(float));
        if (!tmp)
            return JS_ThrowOutOfMemory(ctx);
        simd.gemv(tmp, a, x, m, n, 0.0f);      /* tmp = A*x, y never read */
        if (beta == 0.0) {
            for (i = 0; i < m; i++)
                y[i] = (float)(alpha * tmp[i]);
        } else {
            for (i = 0; i < m; i++)
                y[i] = (float)(alpha * tmp[i] + beta * y[i]);
        }
        free(tmp);
    } else {
        simd.gemv(y, a, x, m, n, (float)beta);
    }
    return JS_DupValue(ctx, argv[0]);
}

/* gemvT(y, a, x, m, n, beta): y = beta*y + A^T*x; A is m x n, x is length m, y is length n. */
static JSValue js_simd_gemv_t(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint64_t m64, n64;
    double beta;
    float *y, *a, *x;
    size_t ny, na, nx, m, n;
    (void)this_val; (void)argc;
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
    if (simd_get_f32(ctx, argv[0], &y, &ny) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != n)
        return JS_ThrowRangeError(ctx, "gemvT: y.length must equal n");
    if (nx != m)
        return JS_ThrowRangeError(ctx, "gemvT: x.length must equal m");
    if (na != m * n)
        return JS_ThrowRangeError(ctx, "gemvT: a.length must equal m*n");
    simd.gemv_t(y, a, x, m, n, (float)beta);
    return JS_DupValue(ctx, argv[0]);
}

/* gemm(c, a, b, m, n, k, alpha, beta): C = alpha*A*B + beta*C;
 * A is m x k, B is k x n, C is m x n. */
static JSValue js_simd_gemm(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint64_t m64, n64, k64;
    double alpha, beta;
    float *c, *a, *b;
    size_t nc, na, nb, m, n, k;
    (void)this_val; (void)argc;
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
    if (simd_get_f32(ctx, argv[0], &c, &nc) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (nc != m * n)
        return JS_ThrowRangeError(ctx, "gemm: c.length must equal m*n");
    if (na != m * k)
        return JS_ThrowRangeError(ctx, "gemm: a.length must equal m*k");
    if (nb != k * n)
        return JS_ThrowRangeError(ctx, "gemm: b.length must equal k*n");
    simd.gemm(c, a, b, m, n, k, (float)alpha, (float)beta);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- clamp / threshold: in-place ---------- */

static JSValue js_simd_clamp(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double lo, hi;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "clamp: window out of range");
        a += _woff; n = _wlen;
    }
    simd.clamp(a, a, (float)lo, (float)hi, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_threshold(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double t;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "threshold: window out of range");
        a += _woff; n = _wlen;
    }
    simd.threshold(a, a, (float)t, n);
    return JS_DupValue(ctx, argv[0]);
}

/* topkIndices(vals, k): returns a fresh Uint32Array of the min(k, vals.length)
 * indices of the LARGEST values (unspecified order -- min-heap selection).
 * simd.topk_indices was fixed at source (dyna-simd-scalar.c) to keep the k
 * largest, so it is called directly on vals. */
static JSValue js_simd_topk_indices(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint64_t k64;
    float *vals;
    size_t n, k;
    uint32_t stub = 0, *idx;
    JSValue ab, out;
    JSValueConst ta_args[3];
    (void)this_val; (void)argc;
    if (JS_ToIndex(ctx, &k64, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &vals, &n))
        return JS_EXCEPTION;
    k = (size_t)k64;
    if (k > n)
        k = n;
    idx = &stub;
    if (k) {
        idx = (uint32_t *)malloc(k * sizeof(uint32_t));
        if (!idx)
            return JS_ThrowOutOfMemory(ctx);
    }
    simd.topk_indices(vals, idx, n, k);
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)idx, k * sizeof(uint32_t));
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

/* ---------- f64 (double-precision) kernels over Float64Array ----------
 * JS Number IS f64, so these run zero-copy on a Float64Array's backing store
 * (bpe==8). Reductions (f64Sum/f64Dot) reorder additions and round slightly
 * differently from a naive sequential sum; f64Min/f64Max/f64Scale/f64Axpy are
 * bit-exact. Reentrancy discipline matches the f32 kernels: any scalar arg is
 * coerced to a C local (which may run user valueOf) BEFORE the buffer is
 * resolved, with no JS between the resolve and the kernel call. */

static JSValue js_simd_f64_sum(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Sum: window out of range");
        a += _woff; n = _wlen;
    }
    return JS_NewFloat64(ctx, simd.f64_sum(a, n));
}

static JSValue js_simd_f64_dot(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &na) || simd_get_f64(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "f64Dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "f64Dot: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "f64Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.f64_dot(a, b, na));
}

static JSValue js_simd_f64_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Max: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Max: empty array");
    /* f64_max has its own 0<n<width scalar guard, so no small-n workaround. */
    return JS_NewFloat64(ctx, simd.f64_max(a, n));
}

static JSValue js_simd_f64_min(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Min: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Min: empty array");
    return JS_NewFloat64(ctx, simd.f64_min(a, n));
}

/* f64Scale(x, s): x[i] *= s, in-place; returns the same array (like scale). */
static JSValue js_simd_f64_scale(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    double *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double s;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Scale: window out of range");
        a += _woff; n = _wlen;
    }
    simd.f64_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* f64Axpy(y, a, x): y[i] += a*x[i], in-place on y; returns y (like axpy). */
static JSValue js_simd_f64_axpy(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double *y, *x;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t ny, nx;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64(ctx, argv[0], &y, &ny) || simd_get_f64(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > ny)
            return JS_ThrowRangeError(ctx, "f64Axpy: window out of range");
        if (_woff + _wlen > nx)
            return JS_ThrowRangeError(ctx, "f64Axpy: window out of range");
        y += _woff; ny = _wlen;
        x += _woff; nx = _wlen;
    }
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "f64Axpy: length mismatch");
    simd.f64_axpy(y, alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- i32 (signed 32-bit integer) kernels over Int32Array ----------
 * Zero-copy over an Int32Array's backing store (element type verified by class
 * id, so a same-stride Float32Array/Uint32Array is rejected, never bit-
 * reinterpreted). i32Sum returns an exact JS Number for |sum| <= 2^53 (the
 * kernel accumulates in int64); i32Dot returns a Number (sum of double
 * products). i32Add/i32Mul write into a separate `out` (like `add`); i32Scale
 * is in-place. Reentrancy: i32Scale's scalar `s` is coerced to a C local (which
 * may run user valueOf) BEFORE any buffer is resolved. */

static JSValue js_simd_i32_sum(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Sum: window out of range");
        a += _woff; n = _wlen;
    }
    return JS_NewInt64(ctx, simd.i32_sum(a, n));
}

static JSValue js_simd_i32_min(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Min: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Min: empty array");
    return JS_NewInt32(ctx, simd.i32_min(a, n));
}

static JSValue js_simd_i32_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Max: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Max: empty array");
    return JS_NewInt32(ctx, simd.i32_max(a, n));
}

static JSValue js_simd_i32_dot(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &na) || simd_get_i32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Dot: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Dot: window out of range");
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (na != nb)
        return JS_ThrowRangeError(ctx, "i32Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.i32_dot(a, b, na));
}

/* i32Add(out, a, b): out[i] = a[i] + b[i] (mod 2^32); returns out. */
static JSValue js_simd_i32_add(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *o, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &o, &no) ||
        simd_get_i32(ctx, argv[1], &a, &na) ||
        simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Add: window out of range");
        o += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "i32Add: length mismatch");
    simd.i32_add(o, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

/* i32Mul(out, a, b): out[i] = a[i] * b[i] low 32 bits (Math.imul); returns out. */
static JSValue js_simd_i32_mul(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *o, *a, *b;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 3, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &o, &no) ||
        simd_get_i32(ctx, argv[1], &a, &na) ||
        simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > no)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        if (_woff + _wlen > na)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        if (_woff + _wlen > nb)
            return JS_ThrowRangeError(ctx, "i32Mul: window out of range");
        o += _woff; no = _wlen;
        a += _woff; na = _wlen;
        b += _woff; nb = _wlen;
    }
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "i32Mul: length mismatch");
    simd.i32_mul(o, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

/* i32Scale(x, s): x[i] = x[i] * s low 32 bits, in-place; returns x. */
static JSValue js_simd_i32_scale(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 2, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    int32_t s;
    (void)this_val; (void)argc;
    if (JS_ToInt32(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Scale: window out of range");
        a += _woff; n = _wlen;
    }
    simd.i32_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- statistics: mean / variance / normalize ----------
 * COMPOSED from the existing reduction kernels, so they inherit the
 * reductions' documented regime: additions are reordered vs a sequential
 * scalar loop (relative-tolerance agreement, not bitwise), NaN in the input
 * propagates to NaN, and every n >= 1 is safe to route into the table (the
 * kernels guard their seed loads; the historical 0 < n < 64 scalar detour is
 * gone). n == 0 is rejected with RangeError like the other mathematically
 * undefined reductions (max/min). A constant array has f32 rounding noise for
 * a centered variance: normalize then divides by that noise's RMS (all-±1-ish
 * output) or, when the residues cancel exactly, by zero (all-NaN, IEEE
 * 0/0) -- a zero-variance input has no meaningful z-score either way. */

static JSValue js_simd_mean(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "mean: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "mean: empty array");
    return JS_NewFloat64(ctx, (double)simd.sum(a, n) / (double)n);
}

/* Two-pass centered variance: tmp = a - mean via the add_s kernel, then
 * dot(tmp, tmp)/n. This is the numerically honest composition -- the
 * kernel-free one-pass form sum(a^2)/n - mean^2 cancels catastrophically
 * when |mean| >> std. One scratch allocation, freed before returning. */
static JSValue js_simd_variance(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    float *a, *tmp;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double m, v;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "variance: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "variance: empty array");
    m = (double)simd.sum(a, n) / (double)n;
    tmp = (float *)malloc(n * sizeof(float));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    simd.add_s(tmp, a, (float)-m, n);
    v = (double)simd.dot(tmp, tmp, n) / (double)n;
    free(tmp);
    return JS_NewFloat64(ctx, v);
}

/* normalize(a): in-place z-score a[i] = (a[i] - mean) / std; returns a. It
 * joins the activation family (softmax & co) in taking ONE array and working
 * IN PLACE -- the out-of-place form would need a third argument slot and the
 * module's convention for single-array transforms is in-place. Composed: the
 * centering runs through add_s, the variance through dot. */
static JSValue js_simd_normalize(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    double m, v, std;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "normalize: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "normalize: empty array");
    m = (double)simd.sum(a, n) / (double)n;
    simd.add_s(a, a, (float)-m, n);
    v = (double)simd.dot(a, a, n) / (double)n;
    std = sqrt(v);
    /* 1/std first, one multiply: there is no scalar-divide kernel slot.
     * std == 0 (exact zero residues) scales by +Inf; 0 * Inf = NaN, so a
     * constant array whose centering cancels exactly normalizes to NaN
     * (IEEE 0/0), not to garbage. */
    simd.mul_s(a, a, (float)(1.0 / std), n);
    return JS_DupValue(ctx, argv[0]);
}

/* f64Mean/f64Variance over Float64Array: same two-pass composition. The table
 * has no f64 scalar-add slot (territory), so the centering pass is a
 * plain scalar loop here in the binding; the reductions still run through the
 * f64_sum/f64_dot kernels. f64 reductions reorder additions vs a sequential
 * scalar loop -- relative tolerance, per the f64 family's header contract. */

static JSValue js_simd_f64_mean(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Mean: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Mean: empty array");
    return JS_NewFloat64(ctx, simd.f64_sum(a, n) / (double)n);
}

static JSValue js_simd_f64_variance(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    double *a, *tmp;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n, i;
    double m, v;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "f64Variance: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Variance: empty array");
    m = simd.f64_sum(a, n) / (double)n;
    tmp = (double *)malloc(n * sizeof(double));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++)
        tmp[i] = a[i] - m;
    v = simd.f64_dot(tmp, tmp, n) / (double)n;
    free(tmp);
    return JS_NewFloat64(ctx, v);
}

/* i32Mean over Int32Array: composed from i32_sum, whose accumulation is
 * EXACT -- int64-widening integer adds are associative and cannot overflow
 * for any array that fits memory (|sum| <= n * 2^31, so int64 saturates only
 * past 2^33 elements = 32 GiB of Int32Array). The int64 -> double conversion
 * at this boundary is exact while |sum| <= 2^53 (n up to ~4M worst-case
 * elements); past that JS Numbers round to nearest double, and the final
 * division is one double rounding -- overflow-safe by construction, with no
 * intermediate that can wrap. */
static JSValue js_simd_i32_mean(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    if (_whas < 0)
        return JS_EXCEPTION;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    /* window: uniform (offset,length) over every array arg. */
    if (_whas) {
        if (_woff + _wlen > n)
            return JS_ThrowRangeError(ctx, "i32Mean: window out of range");
        a += _woff; n = _wlen;
    }
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Mean: empty array");
    return JS_NewFloat64(ctx, (double)simd.i32_sum(a, n) / (double)n);
}

/* ---------- Inclusive prefix scans over Int32Array OR Float32Array ----------
 * Both element types share a 4-byte stride, so the typed-array class id (not
 * bytes-per-element) selects the kernel. In place; returns the same array. */

static JSValue js_simd_cumsum(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSClassID cid;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    (void)this_val; (void)argc;
    if (_whas < 0)
        return JS_EXCEPTION;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t *a;
        size_t n;
        if (simd_get_i32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cumsum: window out of range");
            a += _woff; n = _wlen;
        }
        simd.i32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float *a;
        size_t n;
        if (simd_get_f32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cumsum: window out of range");
            a += _woff; n = _wlen;
        }
        simd.f32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    return JS_ThrowTypeError(ctx, "cumsum: expected an Int32Array or Float32Array");
}

static JSValue js_simd_cummax(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSClassID cid;
    size_t _woff = 0, _wlen = 0;
    int _whas = simd_window(ctx, argc, argv, 1, &_woff, &_wlen);
    (void)this_val; (void)argc;
    if (_whas < 0)
        return JS_EXCEPTION;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t *a;
        size_t n;
        if (simd_get_i32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cummax: window out of range");
            a += _woff; n = _wlen;
        }
        simd.i32_cummax(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float *a;
        size_t n;
        if (simd_get_f32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        if (_whas) {
            if (_woff + _wlen > n)
                return JS_ThrowRangeError(ctx, "cummax: window out of range");
            a += _woff; n = _wlen;
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

    /* reductions */
    JS_CFUNC_DEF("normL1", 1, js_simd_norm_l1),
    JS_CFUNC_DEF("normL2", 1, js_simd_norm_l2),
    JS_CFUNC_DEF("max", 1, js_simd_max),
    JS_CFUNC_DEF("min", 1, js_simd_min),
    JS_CFUNC_DEF("argmax", 1, js_simd_argmax),
    JS_CFUNC_DEF("argmin", 1, js_simd_argmin),

    /* element-wise vector-vector */
    JS_CFUNC_DEF("sub", 3, js_simd_sub),
    JS_CFUNC_DEF("mul", 3, js_simd_mul),
    JS_CFUNC_DEF("div", 3, js_simd_div),
    JS_CFUNC_DEF("abs", 2, js_simd_abs),
    JS_CFUNC_DEF("fma", 3, js_simd_fma),

    /* scalar-vector */
    JS_CFUNC_DEF("addScalar", 2, js_simd_add_s),
    JS_CFUNC_DEF("affine", 3, js_simd_scale_add_s),

    /* activations */
    JS_CFUNC_DEF("sigmoid", 1, js_simd_sigmoid),
    JS_CFUNC_DEF("relu", 1, js_simd_relu),
    JS_CFUNC_DEF("relu6", 1, js_simd_relu6),
    JS_CFUNC_DEF("leakyRelu", 2, js_simd_leaky_relu),
    JS_CFUNC_DEF("elu", 2, js_simd_elu),
    JS_CFUNC_DEF("tanhFast", 1, js_simd_tanh_fast),
    JS_CFUNC_DEF("gelu", 1, js_simd_gelu),
    JS_CFUNC_DEF("silu", 1, js_simd_silu),

    /* softmax family */
    JS_CFUNC_DEF("softmax", 1, js_simd_softmax),
    JS_CFUNC_DEF("logSoftmax", 1, js_simd_log_softmax),

    /* unary math */
    JS_CFUNC_DEF("vexp", 1, js_simd_vexp),
    JS_CFUNC_DEF("vlog", 1, js_simd_vlog),
    JS_CFUNC_DEF("vsqrt", 1, js_simd_vsqrt),
    JS_CFUNC_DEF("vrsqrt", 1, js_simd_vrsqrt),
    JS_CFUNC_DEF("vinv", 1, js_simd_vinv),

    /* distances */
    JS_CFUNC_DEF("distL2", 2, js_simd_dist_l2),
    JS_CFUNC_DEF("distL1", 2, js_simd_dist_l1),
    JS_CFUNC_DEF("distCos", 2, js_simd_dist_cos),
    JS_CFUNC_DEF("distCheb", 2, js_simd_dist_cheb),

    /* BLAS-2/3 */
    JS_CFUNC_DEF("gemv", 6, js_simd_gemv),
    JS_CFUNC_DEF("gemvT", 6, js_simd_gemv_t),
    JS_CFUNC_DEF("gemm", 8, js_simd_gemm),

    /* statistics: mean/variance/normalize per width */
    JS_CFUNC_DEF("mean", 1, js_simd_mean),
    JS_CFUNC_DEF("variance", 1, js_simd_variance),
    JS_CFUNC_DEF("normalize", 1, js_simd_normalize),

    /* comparison / selection */
    JS_CFUNC_DEF("clamp", 3, js_simd_clamp),
    JS_CFUNC_DEF("threshold", 2, js_simd_threshold),
    JS_CFUNC_DEF("topkIndices", 2, js_simd_topk_indices),

    /* f64 (double-precision) over Float64Array */
    JS_CFUNC_DEF("f64Sum", 1, js_simd_f64_sum),
    JS_CFUNC_DEF("f64Dot", 2, js_simd_f64_dot),
    JS_CFUNC_DEF("f64Max", 1, js_simd_f64_max),
    JS_CFUNC_DEF("f64Min", 1, js_simd_f64_min),
    JS_CFUNC_DEF("f64Scale", 2, js_simd_f64_scale),
    JS_CFUNC_DEF("f64Axpy", 3, js_simd_f64_axpy),
    JS_CFUNC_DEF("f64Mean", 1, js_simd_f64_mean),
    JS_CFUNC_DEF("f64Variance", 1, js_simd_f64_variance),

    /* i32 (signed 32-bit integer) over Int32Array */
    JS_CFUNC_DEF("i32Sum", 1, js_simd_i32_sum),
    JS_CFUNC_DEF("i32Min", 1, js_simd_i32_min),
    JS_CFUNC_DEF("i32Max", 1, js_simd_i32_max),
    JS_CFUNC_DEF("i32Dot", 2, js_simd_i32_dot),
    JS_CFUNC_DEF("i32Add", 3, js_simd_i32_add),
    JS_CFUNC_DEF("i32Mul", 3, js_simd_i32_mul),
    JS_CFUNC_DEF("i32Scale", 2, js_simd_i32_scale),
    JS_CFUNC_DEF("i32Mean", 1, js_simd_i32_mean),

    /* inclusive prefix scans over Int32Array OR Float32Array */
    JS_CFUNC_DEF("cumsum", 1, js_simd_cumsum),
    JS_CFUNC_DEF("cummax", 1, js_simd_cummax),
};

static int js_simd_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

/* Class id of a fresh typed array of `type` (0 on failure). Reads the id off a
 * sample instance — built-in ids are process-global constants, so caching the
 * result once is valid for every context in the process. */
static JSClassID simd_capture_cid(JSContext *ctx, JSTypedArrayEnum type)
{
    JSValueConst args[1];
    JSValue ta;
    JSClassID cid = 0;
    args[0] = JS_NewInt32(ctx, 0); /* immediate: new <Type>Array(0) */
    ta = JS_NewTypedArray(ctx, 1, args, type);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return cid;
}

int js_nat_init_simd(JSContext *ctx)
{
    JSModuleDef *m;
    /* Populate the dispatch table before any kernel is called. Without this the
     * `simd` globals are NULL unless another SIMD module (search/text/http)
     * happened to init first -- a fragile cross-module dependency. */
    simd_init();
    /* Cache the Int32Array / Float32Array class ids used to route cumsum/cummax
     * and to strictly type the i32* methods (both element types are 4 bytes). */
    simd_cid_int32 = simd_capture_cid(ctx, JS_TYPED_ARRAY_INT32);
    simd_cid_float32 = simd_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT32);
    m = JS_NewCModule(ctx, "dyna:simd", js_simd_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, js_simd_funcs, countof(js_simd_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SIMD */
