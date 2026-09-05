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

/* Workaround for a pre-existing bug in the shared table, NOT owned by this
 * module: the max/min/argmax/argmin kernels in every accelerated ISA
 * (dyna-simd-{neon,sse42,avx2,avx512,sve}.c) unconditionally load a full
 * SIMD-width vector to seed the reduction (e.g. NEON's plain vld1q_f32(x))
 * BEFORE checking n against the loop bound -- a heap-buffer-overflow read
 * for 0 < n < that ISA's vector width. Every other kernel in the table
 * (sum/dot/norm_l1/softmax/...) guards this correctly with an `i + W <= n`
 * check before the first load; only these four reduction entry points don't.
 * Route small n around the table into a trivial scalar reduction here; 64
 * covers every ISA's widest vector in dyna-simd-kernels.h (AVX-512 is the
 * widest at 16 lanes) with ample margin for any future addition. */
#define SIMD_SAFE_REDUCE_N 64

static float simd_small_max(const float *x, size_t n)
{
    float m = x[0];
    for (size_t i = 1; i < n; i++)
        if (x[i] > m) m = x[i];
    return m;
}

static float simd_small_min(const float *x, size_t n)
{
    float m = x[0];
    for (size_t i = 1; i < n; i++)
        if (x[i] < m) m = x[i];
    return m;
}

static size_t simd_small_argmax(const float *x, size_t n)
{
    size_t idx = 0;
    float m = x[0];
    for (size_t i = 1; i < n; i++)
        if (x[i] > m) { m = x[i]; idx = i; }
    return idx;
}

static size_t simd_small_argmin(const float *x, size_t n)
{
    size_t idx = 0;
    float m = x[0];
    for (size_t i = 1; i < n; i++)
        if (x[i] < m) { m = x[i]; idx = i; }
    return idx;
}

static JSValue js_simd_dot(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "dot: length mismatch");
    return JS_NewFloat64(ctx, simd.dot(a, b, na));
}

static JSValue js_simd_sum(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, simd.sum(a, n));
}

static JSValue js_simd_scale(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double k;
    (void)this_val; (void)argc;
    /* coerce the scalar to a C local FIRST (may run user valueOf), then resolve
     * the buffer -- no JS between resolve and the kernel. */
    if (JS_ToFloat64(ctx, &k, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.mul_s(a, a, (float)k, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_axpy(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *y, *x;
    size_t ny, nx;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &y, &ny) || simd_get_f32(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
    if (ny != nx)
        return JS_ThrowRangeError(ctx, "axpy: length mismatch");
    simd.axpy(y, (float)alpha, x, ny);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_add(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
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
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, simd.norm_l1(a, n));
}

static JSValue js_simd_norm_l2(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, simd.norm_l2(a, n));
}

static JSValue js_simd_max(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "max: empty array");
    return JS_NewFloat64(ctx, n < SIMD_SAFE_REDUCE_N ? simd_small_max(a, n) : simd.max(a, n));
}

static JSValue js_simd_min(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "min: empty array");
    return JS_NewFloat64(ctx, n < SIMD_SAFE_REDUCE_N ? simd_small_min(a, n) : simd.min(a, n));
}

static JSValue js_simd_argmax(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmax: empty array");
    return JS_NewInt64(ctx, (int64_t)(n < SIMD_SAFE_REDUCE_N ? simd_small_argmax(a, n) : simd.argmax(a, n)));
}

static JSValue js_simd_argmin(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "argmin: empty array");
    return JS_NewInt64(ctx, (int64_t)(n < SIMD_SAFE_REDUCE_N ? simd_small_argmin(a, n) : simd.argmin(a, n)));
}

/* ---------- element-wise: vector-vector -> vector (out separate, like add) ---------- */

static JSValue js_simd_sub(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "sub: length mismatch");
    simd.sub(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_mul(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "mul: length mismatch");
    simd.mul(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_div(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
    if (no != na || na != nb)
        return JS_ThrowRangeError(ctx, "div: length mismatch");
    simd.div(out, a, b, no);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_abs(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *out, *in;
    size_t no, ni;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &out, &no) ||
        simd_get_f32(ctx, argv[1], &in, &ni))
        return JS_EXCEPTION;
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
    size_t nz, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &z, &nz) ||
        simd_get_f32(ctx, argv[1], &a, &na) ||
        simd_get_f32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
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
    size_t n;
    double s;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.add_s(a, a, (float)s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* affine(a, alpha, beta): a = alpha*a + beta, in-place. */
static JSValue js_simd_scale_add_s(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double alpha, beta;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &beta, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.scale_add_s(a, (float)alpha, a, (float)beta, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- activations: in-place, one array in/out (header-documented safe) ---------- */

static JSValue js_simd_sigmoid(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.sigmoid(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.relu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_relu6(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.relu6(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_leaky_relu(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double slope;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &slope, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.leaky_relu(a, a, (float)slope, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_elu(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.elu(a, a, (float)alpha, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_tanh_fast(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.tanh_fast(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_gelu(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
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
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.silu(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- softmax family: in-place; empty input would OOB-read in[0] in the
 * kernel, so reject it here rather than passing it through. ---------- */

static JSValue js_simd_softmax(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "softmax: empty array");
    simd.softmax(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_log_softmax(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
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
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.vexp(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vlog(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.vlog(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vsqrt(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.vsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vrsqrt(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.vrsqrt(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_vinv(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.vinv(a, a, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- distances: vector-vector -> scalar ---------- */

static JSValue js_simd_dist_l2(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL2: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_l2(a, b, na));
}

static JSValue js_simd_dist_l1(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distL1: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_l1(a, b, na));
}

static JSValue js_simd_dist_cos(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCos: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_cos(a, b, na));
}

static JSValue js_simd_dist_cheb(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f32(ctx, argv[0], &a, &na) || simd_get_f32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "distCheb: length mismatch");
    return JS_NewFloat64(ctx, simd.dist_cheb(a, b, na));
}

/* ---------- BLAS-2/3: row-major, explicit dims, in-place on the output arg.
 * Dims/scalars are ALL coerced first (JS_ToIndex/JS_ToFloat64), before any
 * buffer is resolved -- same reentrancy rule as scale/axpy/add. ---------- */

/* gemv(y, a, x, m, n, beta): y = beta*y + A*x; A is m x n, x is length n, y is length m. */
static JSValue js_simd_gemv(JSContext *ctx, JSValueConst this_val,
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
    simd.gemv(y, a, x, m, n, (float)beta);
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
    size_t n;
    double lo, hi;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &lo, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &hi, argv[2]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.clamp(a, a, (float)lo, (float)hi, n);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_simd_threshold(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float *a;
    size_t n;
    double t;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
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
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, simd.f64_sum(a, n));
}

static JSValue js_simd_f64_dot(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &na) || simd_get_f64(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "f64Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.f64_dot(a, b, na));
}

static JSValue js_simd_f64_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Max: empty array");
    /* f64_max has its own 0<n<width scalar guard, so no small-n workaround. */
    return JS_NewFloat64(ctx, simd.f64_max(a, n));
}

static JSValue js_simd_f64_min(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    double *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "f64Min: empty array");
    return JS_NewFloat64(ctx, simd.f64_min(a, n));
}

/* f64Scale(x, s): x[i] *= s, in-place; returns the same array (like scale). */
static JSValue js_simd_f64_scale(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    double *a;
    size_t n;
    double s;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.f64_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* f64Axpy(y, a, x): y[i] += a*x[i], in-place on y; returns y (like axpy). */
static JSValue js_simd_f64_axpy(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double *y, *x;
    size_t ny, nx;
    double alpha;
    (void)this_val; (void)argc;
    if (JS_ToFloat64(ctx, &alpha, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_f64(ctx, argv[0], &y, &ny) || simd_get_f64(ctx, argv[2], &x, &nx))
        return JS_EXCEPTION;
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
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, simd.i32_sum(a, n));
}

static JSValue js_simd_i32_min(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Min: empty array");
    return JS_NewInt32(ctx, simd.i32_min(a, n));
}

static JSValue js_simd_i32_max(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a;
    size_t n;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    if (n == 0)
        return JS_ThrowRangeError(ctx, "i32Max: empty array");
    return JS_NewInt32(ctx, simd.i32_max(a, n));
}

static JSValue js_simd_i32_dot(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *a, *b;
    size_t na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &a, &na) || simd_get_i32(ctx, argv[1], &b, &nb))
        return JS_EXCEPTION;
    if (na != nb)
        return JS_ThrowRangeError(ctx, "i32Dot: length mismatch");
    return JS_NewFloat64(ctx, simd.i32_dot(a, b, na));
}

/* i32Add(out, a, b): out[i] = a[i] + b[i] (mod 2^32); returns out. */
static JSValue js_simd_i32_add(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int32_t *o, *a, *b;
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &o, &no) ||
        simd_get_i32(ctx, argv[1], &a, &na) ||
        simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
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
    size_t no, na, nb;
    (void)this_val; (void)argc;
    if (simd_get_i32(ctx, argv[0], &o, &no) ||
        simd_get_i32(ctx, argv[1], &a, &na) ||
        simd_get_i32(ctx, argv[2], &b, &nb))
        return JS_EXCEPTION;
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
    size_t n;
    int32_t s;
    (void)this_val; (void)argc;
    if (JS_ToInt32(ctx, &s, argv[1]))
        return JS_EXCEPTION;
    if (simd_get_i32(ctx, argv[0], &a, &n))
        return JS_EXCEPTION;
    simd.i32_scale(a, a, s, n);
    return JS_DupValue(ctx, argv[0]);
}

/* ---------- Inclusive prefix scans over Int32Array OR Float32Array ----------
 * Both element types share a 4-byte stride, so the typed-array class id (not
 * bytes-per-element) selects the kernel. In place; returns the same array. */

static JSValue js_simd_cumsum(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSClassID cid;
    (void)this_val; (void)argc;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t *a;
        size_t n;
        if (simd_get_i32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        simd.i32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float *a;
        size_t n;
        if (simd_get_f32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        simd.f32_cumsum(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    return JS_ThrowTypeError(ctx, "cumsum: expected an Int32Array or Float32Array");
}

static JSValue js_simd_cummax(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSClassID cid;
    (void)this_val; (void)argc;
    JS_GetAnyOpaque(argv[0], &cid);
    if (cid == (JSClassID)simd_cid_int32) {
        int32_t *a;
        size_t n;
        if (simd_get_i32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
        simd.i32_cummax(a, a, n);
        return JS_DupValue(ctx, argv[0]);
    }
    if (cid == (JSClassID)simd_cid_float32) {
        float *a;
        size_t n;
        if (simd_get_f32(ctx, argv[0], &a, &n))
            return JS_EXCEPTION;
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

    /* i32 (signed 32-bit integer) over Int32Array */
    JS_CFUNC_DEF("i32Sum", 1, js_simd_i32_sum),
    JS_CFUNC_DEF("i32Min", 1, js_simd_i32_min),
    JS_CFUNC_DEF("i32Max", 1, js_simd_i32_max),
    JS_CFUNC_DEF("i32Dot", 2, js_simd_i32_dot),
    JS_CFUNC_DEF("i32Add", 3, js_simd_i32_add),
    JS_CFUNC_DEF("i32Mul", 3, js_simd_i32_mul),
    JS_CFUNC_DEF("i32Scale", 2, js_simd_i32_scale),

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
