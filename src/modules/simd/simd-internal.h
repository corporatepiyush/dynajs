#ifndef SIMD_INTERNAL_H
#define SIMD_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SIMD)

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#include "core/dyn-simdk.h"

int simd_f32_has_nonpositive(const float* a, size_t n);

size_t simd_f32_first_equal(const float* a, size_t n, float v);

int simd_get_f32(JSContext* ctx, JSValueConst v, float** pp, size_t* pn);

int simd_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn);

int simd_get_i32(JSContext* ctx, JSValueConst v, int32_t** pp, size_t* pn);

static inline int simd_dims_overflow(size_t a, size_t b)
{
    return a != 0 && b > (size_t)-1 / a;
}

#define SIMD_NAN_SCAN_BLOCK 64

int simd_f32_has_nan(const float* a, size_t n);

#define SIMD_F32_WIDE_LANES_BIG 16

static inline __attribute__((always_inline)) double simd_f32_dot_lanes(const float* a, const float* b, size_t n, size_t L)
{
    double acc[SIMD_F32_WIDE_LANES_BIG] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(L - 1);
    double total = 0.0;
    for (; i < body; i += L)
        for (k = 0; k < L; k++)
            acc[k] += (double)a[i + k] * (double)b[i + k];
    for (k = 0; k < L; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)a[i] * (double)b[i];
    return total;
}

double simd_f32_dot_wide(const float* a, const float* b, size_t n);

static inline __attribute__((always_inline)) double simd_f32_centered_sq_lanes(const float* a, float shift, size_t n, size_t L)
{
    double acc[SIMD_F32_WIDE_LANES_BIG] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(L - 1);
    double total = 0.0;
    for (; i < body; i += L)
        for (k = 0; k < L; k++) {
            float t = a[i + k] + shift;
            acc[k] += (double)t * (double)t;
        }
    for (k = 0; k < L; k++)
        total += acc[k];
    for (; i < n; i++) {
        float t = a[i] + shift;
        total += (double)t * (double)t;
    }
    return total;
}

double simd_f32_centered_sq_wide(const float* a, float shift, size_t n);

static inline __attribute__((always_inline)) double simd_f32_sum_lanes(const float* a, size_t n, size_t L)
{
    double acc[SIMD_F32_WIDE_LANES_BIG] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(L - 1);
    double total = 0.0;
    for (; i < body; i += L)
        for (k = 0; k < L; k++)
            acc[k] += (double)a[i + k];
    for (k = 0; k < L; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)a[i];
    return total;
}

double simd_f32_sum_wide(const float* a, size_t n);

static inline __attribute__((always_inline)) double simd_f32_norm_l1_lanes(const float* a, size_t n, size_t L)
{
    double acc[SIMD_F32_WIDE_LANES_BIG] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(L - 1);
    double total = 0.0;
    for (; i < body; i += L)
        for (k = 0; k < L; k++)
            acc[k] += (double)fabsf(a[i + k]);
    for (k = 0; k < L; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)fabsf(a[i]);
    return total;
}

double simd_f32_norm_l1_wide(const float* a, size_t n);

void simd_f32_unary_same_kernel(void (*kernel)(float*, const float*, size_t),
    float* a, size_t n);

size_t simd_f32_arg_skip_nan(const float* a, size_t n, int want_max);

static inline int simd_spans_overlap(const void* p, size_t pbytes, const void* q,
    size_t qbytes)
{
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)q;
    return pbytes && qbytes && a < b + qbytes && b < a + pbytes;
}

int simd_window(JSContext* ctx, int argc, JSValueConst* argv, int base,
    size_t* poff, size_t* plen);

int simd_get_f32_w(JSContext* ctx, JSValueConst v, float** pp, size_t* pn);

static inline int simd_get_f64_w(JSContext* ctx, JSValueConst v, double** pp, size_t* pn)
{
    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    return simd_get_f64(ctx, v, pp, pn);
}

static inline int simd_get_i32_w(JSContext* ctx, JSValueConst v, int32_t** pp, size_t* pn)
{
    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    return simd_get_i32(ctx, v, pp, pn);
}

JSValue js_simd_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_axpy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_scale_add_s(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_dist_l2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_dist_l1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_dist_cos(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_dist_cheb(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_gemv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_gemv_t(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_axpy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_min(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_max(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_dot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_scale(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_f64_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_simd_i32_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
