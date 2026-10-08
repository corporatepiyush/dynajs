#ifndef DYNAJS_SIMD_KERNELS_H
#define DYNAJS_SIMD_KERNELS_H

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "core/dyn-twoway.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#if (defined(__GNUC__) || defined(__clang__)) && !defined(DYN_SIMD_FORCE_PORTABLE)
#define DYN_SIMD_GNU 1
#else
#define DYN_SIMD_GNU 0
#endif

#if DYN_SIMD_GNU
#define DYN_SIMD_TARGET(x) __attribute__((target(x)))
#define DYN_SIMD_ALIGNED(n) __attribute__((aligned(n)))
#define DYN_SIMD_UNUSED __attribute__((unused))
#else
#define DYN_SIMD_TARGET(x)
#define DYN_SIMD_ALIGNED(n)
#define DYN_SIMD_UNUSED
#endif

#ifndef unlikely
#if DYN_SIMD_GNU
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define unlikely(x) (!!(x))
#endif
#endif
#ifndef likely
#if DYN_SIMD_GNU
#define likely(x) __builtin_expect(!!(x), 1)
#else
#define likely(x) (!!(x))
#endif
#endif

#if DYN_SIMD_GNU
#define dyn_popcount32(x) __builtin_popcount(x)
#define dyn_popcount64(x) __builtin_popcountll(x)
#define dyn_ctz32(x) __builtin_ctz(x)
#define dyn_ctz64(x) __builtin_ctzll(x)
#else
static inline int dyn_popcount32(uint32_t v)
{
    int c = 0;
    while (v) {
        c += (int)(v & 1u);
        v >>= 1;
    }
    return c;
}
static inline int dyn_popcount64(uint64_t v)
{
    return dyn_popcount32((uint32_t)v) + dyn_popcount32((uint32_t)(v >> 32));
}
static inline int dyn_ctz32(uint32_t v)
{
    int c = 0;
    if (!v)
        return 32;
    while (!(v & 1u)) {
        c++;
        v >>= 1;
    }
    return c;
}
static inline int dyn_ctz64(uint64_t v)
{
    if (!v)
        return 64;
    return (v & 0xffffffffu) ? dyn_ctz32((uint32_t)v)
                             : 32 + dyn_ctz32((uint32_t)(v >> 32));
}
#endif

typedef enum {
    CPU_SSE42 = 1u << 0,
    CPU_AVX2 = 1u << 1,
    CPU_AVX512F = 1u << 2,
    CPU_AVX512BW = 1u << 3,
    CPU_AVX512DQ = 1u << 4,
    CPU_NEON = 1u << 5,
    CPU_SVE = 1u << 6,
} cpu_feature_t;

uint64_t cpu_features(void);

typedef struct simd {
    float (*dot_f)(const float* restrict a, const float* restrict b,
        size_t n);
    float (*dot)(const float* restrict a, const float* restrict b,
        size_t n);
    float (*norm_l2_sq)(const float* restrict x, size_t n);
    float (*norm_l2)(const float* restrict x, size_t n);
    float (*norm_l1)(const float* restrict x, size_t n);
    void (*axpy)(float* restrict y, float alpha, const float* restrict x,
        size_t n);
    void (*axpby)(float* restrict z, float a, const float* restrict x,
        float b, const float* restrict y, size_t n);

    float (*sum)(const float* restrict x, size_t n);
    float (*max)(const float* restrict x, size_t n);
    float (*min)(const float* restrict x, size_t n);
    size_t (*argmax)(const float* restrict x, size_t n);
    size_t (*argmin)(const float* restrict x, size_t n);
    void (*argminmax)(const float* restrict x, size_t n, size_t* argmin_out,
        size_t* argmax_out);

    void (*add)(float* z, const float* restrict a,
        const float* restrict b, size_t n);
    void (*sub)(float* z, const float* restrict a,
        const float* restrict b, size_t n);
    void (*mul)(float* z, const float* restrict a,
        const float* restrict b, size_t n);
    void (*div)(float* restrict z, const float* restrict a,
        const float* restrict b, size_t n);
    void (*abs)(float* restrict out, const float* restrict in, size_t n);
    void (*fma)(float* restrict z, const float* restrict a,
        const float* restrict b, size_t n);

    void (*add_s)(float* z, const float* x, float s,
        size_t n);
    void (*mul_s)(float* z, const float* x, float s,
        size_t n);
    void (*scale_add_s)(float* z, float alpha,
        const float* x, float beta, size_t n);

    void (*sigmoid)(float* out, const float* in,
        size_t n);
    void (*relu)(float* out, const float* in, size_t n);
    void (*relu6)(float* out, const float* in,
        size_t n);
    void (*leaky_relu)(float* out, const float* in,
        float slope, size_t n);
    void (*elu)(float* out, const float* in,
        float alpha, size_t n);
    void (*tanh_fast)(float* out, const float* in,
        size_t n);
    void (*gelu)(float* out, const float* in, size_t n);
    void (*silu)(float* out, const float* in, size_t n);

    void (*softmax)(float* out, const float* in,
        size_t n);
    void (*log_softmax)(float* out, const float* in,
        size_t n);

    void (*vexp)(float* out, const float* in, size_t n);
    void (*vlog)(float* out, const float* in, size_t n);
    void (*vsqrt)(float* out, const float* in,
        size_t n);
    void (*vrsqrt)(float* out, const float* in,
        size_t n);
    void (*vinv)(float* out, const float* in, size_t n);

    float (*dist_l2_sq)(const float* restrict a, const float* restrict b,
        size_t d);
    float (*dist_l1)(const float* restrict a, const float* restrict b,
        size_t d);
    float (*dist_cos)(const float* restrict a, const float* restrict b,
        size_t d);
    float (*dist_cheb)(const float* restrict a, const float* restrict b,
        size_t d);
    float (*dist_l2)(const float* restrict a, const float* restrict b,
        size_t d);

    void (*dist_matrix_l2_sq)(float* restrict out,
        const float* restrict a,
        const float* restrict b, size_t n, size_t m,
        size_t d);
    void (*dist_matrix_cos)(float* restrict out, const float* restrict a,
        const float* restrict b, size_t n, size_t m,
        size_t d);
    void (*dist_matrix_l1)(float* restrict out, const float* restrict a,
        const float* restrict b, size_t n, size_t m,
        size_t d);

    void (*gemv)(float* restrict y, const float* restrict a,
        const float* restrict x, size_t m, size_t n, float beta);
    void (*gemv_t)(float* restrict y, const float* restrict a,
        const float* restrict x, size_t m, size_t n, float beta);

    void (*gemm)(float* restrict c, const float* restrict a,
        const float* restrict b, size_t m, size_t n, size_t k,
        float alpha, float beta);

    void (*threshold)(float* out, const float* in,
        float t, size_t n);
    void (*threshold_sign)(float* restrict out, const float* restrict in,
        float t, size_t n);

    float (*hamming)(const uint32_t* restrict a,
        const uint32_t* restrict b, size_t n_words);

    float (*sigmoid_f)(float x);
    float (*tanh_f)(float x);
    float (*exp_f)(float x);

    void (*topk_indices)(const float* restrict vals,
        uint32_t* restrict indices, size_t n, size_t k);

    void (*clamp)(float* out, const float* in, float lo,
        float hi, size_t n);

    double (*f64_sum)(const double* restrict x, size_t n);
    double (*f64_dot)(const double* restrict a, const double* restrict b,
        size_t n);
    double (*f64_min)(const double* restrict x, size_t n);
    double (*f64_max)(const double* restrict x, size_t n);
    void (*f64_scale)(double* out, const double* x, double s,
        size_t n);
    void (*f64_axpy)(double* restrict y, double a, const double* restrict x,
        size_t n);

    int64_t (*i32_sum)(const int32_t* restrict x, size_t n);
    int32_t (*i32_min)(const int32_t* restrict x, size_t n);
    int32_t (*i32_max)(const int32_t* restrict x, size_t n);
    double (*i32_dot)(const int32_t* restrict a, const int32_t* restrict b,
        size_t n);
    void (*i32_add)(int32_t* restrict out, const int32_t* restrict a,
        const int32_t* restrict b, size_t n);
    void (*i32_mul)(int32_t* restrict out, const int32_t* restrict a,
        const int32_t* restrict b, size_t n);
    void (*i32_scale)(int32_t* out, const int32_t* x, int32_t s,
        size_t n);

    void (*f32_cumsum)(float* out, const float* x, size_t n);
    void (*i32_cumsum)(int32_t* out, const int32_t* x, size_t n);
    void (*f32_cummax)(float* out, const float* x, size_t n);
    void (*i32_cummax)(int32_t* out, const int32_t* x, size_t n);

    size_t (*find_u8)(const uint8_t* restrict p, uint8_t v, size_t n);
    size_t (*find_u16)(const uint16_t* restrict p, uint16_t v, size_t n);
    size_t (*find_u32)(const uint32_t* restrict p, uint32_t v, size_t n);
    size_t (*find_f32)(const float* restrict p, float v, size_t n);
    size_t (*find_f64)(const double* restrict p, double v, size_t n);

    size_t (*strfind)(const uint8_t* text, size_t n, const uint8_t* pat,
        size_t m);

    size_t (*count_u8)(const uint8_t* restrict p, uint8_t v, size_t n);
    size_t (*find_first_of)(const uint8_t* restrict p, size_t n,
        const uint8_t* restrict set, size_t setlen);
    size_t (*find_first_of_u16)(const uint16_t* restrict p, size_t n,
        const uint16_t* restrict set, size_t setlen);
    size_t (*find_bitmap)(const uint8_t* restrict p, size_t n,
        const uint8_t* restrict bitmap);
    size_t (*validate_utf8)(const uint8_t* restrict p, size_t n);

    size_t (*base64_encode)(const uint8_t* restrict src, size_t n,
        char* restrict dst);
    size_t (*base64_decode)(const char* restrict src, size_t n,
        uint8_t* restrict dst);

    void (*hex_encode)(const uint8_t* restrict src, size_t n,
        char* restrict dst);
    size_t (*hex_decode)(const char* restrict src, size_t n,
        uint8_t* restrict dst);

    size_t (*latin1_to_utf8)(const uint8_t* restrict src, size_t n,
        uint8_t* restrict dst);
    int (*utf8_to_latin1)(const uint8_t* restrict src, size_t n,
        uint8_t* restrict dst, size_t* out_len);
    size_t (*count_utf8)(const uint8_t* restrict p, size_t n);

    int (*utf8_to_utf16le)(const uint8_t* restrict src, size_t n,
        uint16_t* restrict dst, size_t* out_units);
    int (*utf16le_to_utf8)(const uint16_t* restrict src, size_t units,
        uint8_t* restrict dst, size_t* out_len);
    int (*validate_utf16le)(const uint16_t* restrict src, size_t units);
    size_t (*count_utf16)(const uint16_t* restrict src, size_t units);

} simd_t;

extern simd_t simd;
extern simd_t simd_scalar;

void simd_init(void);

size_t dyn_strfind_twoway_u8(const uint8_t* text, size_t n, const uint8_t* pat, size_t m);

int simd_strfind_fail(size_t* fails, size_t scanned, size_t m);

static inline size_t simd_strfind_resume(const uint8_t* text, size_t n, const uint8_t* pat, size_t m, size_t from)
{
    size_t r;
    if (from > n)
        return SIZE_MAX;
    r = dyn_strfind_twoway_u8(text + from, n - from, pat, m);
    return r == SIZE_MAX ? SIZE_MAX : from + r;
}

#define SIMD_F32_WIDE_ACC 4
_Static_assert((SIMD_F32_WIDE_ACC & (SIMD_F32_WIDE_ACC - 1)) == 0, "f32 widening accumulator count must be a power of two");

static inline double simd_f32_sum_f64(const float* x, size_t n)
{
    double acc[SIMD_F32_WIDE_ACC] = { 0 };
    size_t i = 0, k, body = n & ~(size_t)(SIMD_F32_WIDE_ACC - 1);
    double total = 0.0;
    for (; i < body; i += SIMD_F32_WIDE_ACC)
        for (k = 0; k < SIMD_F32_WIDE_ACC; k++)
            acc[k] += (double)x[i + k];
    for (k = 0; k < SIMD_F32_WIDE_ACC; k++)
        total += acc[k];
    for (; i < n; i++)
        total += (double)x[i];
    return total;
}

static inline float simd_hsum_f32(const float* x, size_t n)
{
    float acc = 0.0f;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        acc += x[i] + x[i + 1] + x[i + 2] + x[i + 3] + x[i + 4] + x[i + 5] + x[i + 6] + x[i + 7];
    for (; i < n; i++)
        acc += x[i];
    return acc;
}

void simd_override_scalar(simd_t* t);
void simd_override_sse42(simd_t* t);
void simd_override_avx2(simd_t* t);
void simd_override_avx512(simd_t* t);
void simd_override_neon(simd_t* t);
void simd_override_sve(simd_t* t);

void simd_scalar_topk_indices(const float* restrict vals,
    uint32_t* restrict indices, size_t n,
    size_t k);

static inline float fast_exp(float x)
{
    if (unlikely(x != x))
        return x;
    if (unlikely(x < -88.0f))
        return 0.0f;
    if (unlikely(x > 88.0f))
        return INFINITY;
    union {
        float f;
        int32_t i;
    } u;
    float p = 12102203.0f * x;
    p += 1065353216.0f;
    u.i = (int32_t)p;
    return u.f;
}

static inline float fast_sigmoid(float x)
{
    if (unlikely(x < -30.0f))
        return 0.0f;
    if (unlikely(x > 30.0f))
        return 1.0f;
    float ex = fast_exp(-x);
    return 1.0f / (1.0f + ex);
}

static inline float fast_tanh(float x)
{
    if (unlikely(x < -10.0f))
        return -1.0f;
    if (unlikely(x > 10.0f))
        return 1.0f;
    return 2.0f * fast_sigmoid(2.0f * x) - 1.0f;
}

static inline float fast_silu(float x)
{
    return x * (1.0f / (1.0f + fast_exp(-x)));
}

static inline bool is_pow2(size_t v) { return v && !(v & (v - 1)); }
static inline size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

float simd_scalar_dot_f(const float* restrict a, const float* restrict b, size_t n);
float simd_scalar_dot(const float* restrict a, const float* restrict b, size_t n);
float simd_scalar_norm_l2_sq(const float* restrict x, size_t n);
float simd_scalar_norm_l2(const float* restrict x, size_t n);
float simd_scalar_norm_l1(const float* restrict x, size_t n);
void simd_scalar_axpy(float* restrict y, float alpha, const float* restrict x, size_t n);
void simd_scalar_axpby(float* restrict z, float a, const float* restrict x, float b, const float* restrict y, size_t n);
float simd_scalar_sum(const float* restrict x, size_t n);
float simd_scalar_max(const float* restrict x, size_t n);
float simd_scalar_min(const float* restrict x, size_t n);
size_t simd_scalar_argmax(const float* restrict x, size_t n);
size_t simd_scalar_argmin(const float* restrict x, size_t n);
void simd_scalar_argminmax(const float* restrict x, size_t n, size_t* argmin_out, size_t* argmax_out);
void simd_scalar_add(float* z, const float* restrict a, const float* restrict b, size_t n);
void simd_scalar_sub(float* z, const float* restrict a, const float* restrict b, size_t n);
void simd_scalar_mul(float* z, const float* restrict a, const float* restrict b, size_t n);
void simd_scalar_div(float* restrict z, const float* restrict a, const float* restrict b, size_t n);
void simd_scalar_abs(float* restrict out, const float* restrict in, size_t n);
void simd_scalar_fma(float* restrict z, const float* restrict a, const float* restrict b, size_t n);
void simd_scalar_add_s(float* z, const float* x, float s, size_t n);
void simd_scalar_mul_s(float* z, const float* x, float s, size_t n);
void simd_scalar_scale_add_s(float* z, float alpha, const float* x, float beta, size_t n);
void simd_scalar_sigmoid(float* out, const float* in, size_t n);
void simd_scalar_relu(float* out, const float* in, size_t n);
void simd_scalar_relu6(float* out, const float* in, size_t n);
void simd_scalar_leaky_relu(float* out, const float* in, float slope, size_t n);
void simd_scalar_elu(float* out, const float* in, float alpha, size_t n);
void simd_scalar_tanh_fast(float* out, const float* in, size_t n);
void simd_scalar_gelu(float* out, const float* in, size_t n);
void simd_scalar_silu(float* out, const float* in, size_t n);
void simd_scalar_softmax(float* out, const float* in, size_t n);
void simd_scalar_log_softmax(float* out, const float* in, size_t n);
void simd_scalar_vexp(float* out, const float* in, size_t n);
void simd_scalar_vlog(float* out, const float* in, size_t n);
void simd_scalar_vsqrt(float* out, const float* in, size_t n);
void simd_scalar_vrsqrt(float* out, const float* in, size_t n);
void simd_scalar_vinv(float* out, const float* in, size_t n);
float simd_scalar_dist_l2_sq(const float* restrict a, const float* restrict b, size_t d);
float simd_scalar_dist_l1(const float* restrict a, const float* restrict b, size_t d);
float simd_scalar_dist_cos(const float* restrict a, const float* restrict b, size_t d);
float simd_scalar_dist_cheb(const float* restrict a, const float* restrict b, size_t d);
float simd_scalar_dist_l2(const float* restrict a, const float* restrict b, size_t d);
void simd_scalar_dist_matrix_l2_sq(float* restrict out, const float* restrict a, const float* restrict b, size_t n, size_t m, size_t d);
void simd_scalar_dist_matrix_cos(float* restrict out, const float* restrict a, const float* restrict b, size_t n, size_t m, size_t d);
void simd_scalar_dist_matrix_l1(float* restrict out, const float* restrict a, const float* restrict b, size_t n, size_t m, size_t d);
void simd_scalar_gemv(float* restrict y, const float* restrict a, const float* restrict x, size_t m, size_t n, float beta);
void simd_scalar_gemv_t(float* restrict y, const float* restrict a, const float* restrict x, size_t m, size_t n, float beta);
void simd_scalar_gemm(float* restrict c, const float* restrict a, const float* restrict b, size_t m, size_t n, size_t k, float alpha, float beta);
void simd_scalar_threshold(float* out, const float* in, float t, size_t n);
void simd_scalar_threshold_sign(float* restrict out, const float* restrict in, float t, size_t n);
float simd_scalar_hamming(const uint32_t* restrict a, const uint32_t* restrict b, size_t n_words);
float simd_scalar_sigmoid_f(float x);
float simd_scalar_tanh_f(float x);
float simd_scalar_exp_f(float x);
void simd_scalar_clamp(float* out, const float* in, float lo, float hi, size_t n);
double simd_scalar_f64_sum(const double* restrict x, size_t n);
double simd_scalar_f64_dot(const double* restrict a, const double* restrict b, size_t n);
double simd_scalar_f64_min(const double* restrict x, size_t n);
double simd_scalar_f64_max(const double* restrict x, size_t n);
void simd_scalar_f64_scale(double* out, const double* x, double s, size_t n);
void simd_scalar_f64_axpy(double* restrict y, double a, const double* restrict x, size_t n);
int64_t simd_scalar_i32_sum(const int32_t* restrict x, size_t n);
int32_t simd_scalar_i32_min(const int32_t* restrict x, size_t n);
int32_t simd_scalar_i32_max(const int32_t* restrict x, size_t n);
double simd_scalar_i32_dot(const int32_t* restrict a, const int32_t* restrict b, size_t n);
void simd_scalar_i32_add(int32_t* restrict out, const int32_t* restrict a, const int32_t* restrict b, size_t n);
void simd_scalar_i32_mul(int32_t* restrict out, const int32_t* restrict a, const int32_t* restrict b, size_t n);
void simd_scalar_i32_scale(int32_t* out, const int32_t* x, int32_t s, size_t n);
void simd_scalar_f32_cumsum(float* out, const float* x, size_t n);
void simd_scalar_i32_cumsum(int32_t* out, const int32_t* x, size_t n);
void simd_scalar_f32_cummax(float* out, const float* x, size_t n);
void simd_scalar_i32_cummax(int32_t* out, const int32_t* x, size_t n);

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#endif