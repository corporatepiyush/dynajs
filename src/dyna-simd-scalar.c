#include <math.h>
#include "dyna-simd-kernels.h"
#include <string.h>
#include <float.h>
#include <math.h>

float simd_scalar_dot_f(const float* restrict a,
    const float* restrict b, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += (double)a[i] * (double)b[i];
    return (float)acc;
}

float simd_scalar_dot(const float* restrict a,
    const float* restrict b, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; i++)
        acc += a[i] * b[i];
    return acc;
}

float simd_scalar_norm_l2_sq(const float* restrict x, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; i++)
        acc += x[i] * x[i];
    return acc;
}

float simd_scalar_norm_l2(const float* restrict x, size_t n)
{
    return sqrtf(simd_scalar_norm_l2_sq(x, n));
}

float simd_scalar_norm_l1(const float* restrict x, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; i++)
        acc += fabsf(x[i]);
    return acc;
}

void simd_scalar_axpy(float* restrict y, float alpha,
    const float* restrict x, size_t n)
{
    for (size_t i = 0; i < n; i++)
        y[i] += alpha * x[i];
}

void simd_scalar_axpby(float* restrict z, float a,
    const float* restrict x, float b,
    const float* restrict y, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = a * x[i] + b * y[i];
}

float simd_scalar_sum(const float* restrict x, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += (double)x[i];
    return (float)acc;
}

float simd_scalar_max(const float* restrict x, size_t n)
{
    float m = n ? -INFINITY : -FLT_MAX;
    for (size_t i = 0; i < n; i++)
        if (x[i] > m)
            m = x[i];
    return m;
}

float simd_scalar_min(const float* restrict x, size_t n)
{
    float m = n ? INFINITY : FLT_MAX;
    for (size_t i = 0; i < n; i++)
        if (x[i] < m)
            m = x[i];
    return m;
}

size_t simd_scalar_argmax(const float* restrict x, size_t n)
{
    if (n == 0)
        return 0;
    size_t idx = 0;
    float m = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] > m) {
            m = x[i];
            idx = i;
        }
    }
    return idx;
}

size_t simd_scalar_argmin(const float* restrict x, size_t n)
{
    if (n == 0)
        return 0;
    size_t idx = 0;
    float m = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] < m) {
            m = x[i];
            idx = i;
        }
    }
    return idx;
}

void simd_scalar_argminmax(const float* restrict x, size_t n,
    size_t* argmin_out, size_t* argmax_out)
{
    if (n == 0) {
        *argmin_out = *argmax_out = 0;
        return;
    }
    size_t imin = 0, imax = 0;
    float vmin = x[0], vmax = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] < vmin) {
            vmin = x[i];
            imin = i;
        }
        if (x[i] > vmax) {
            vmax = x[i];
            imax = i;
        }
    }
    *argmin_out = imin;
    *argmax_out = imax;
}

void simd_scalar_add(float* z, const float* restrict a,
    const float* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = a[i] + b[i];
}

void simd_scalar_sub(float* z, const float* restrict a,
    const float* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = a[i] - b[i];
}

void simd_scalar_mul(float* z, const float* restrict a,
    const float* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = a[i] * b[i];
}

void simd_scalar_div(float* restrict z, const float* restrict a,
    const float* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = a[i] / b[i];
}

void simd_scalar_abs(float* restrict out,
    const float* restrict in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = fabsf(in[i]);
}

void simd_scalar_fma(float* restrict z, const float* restrict a,
    const float* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] += a[i] * b[i];
}

void simd_scalar_add_s(float* z,
    const float* x, float s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = x[i] + s;
}

void simd_scalar_mul_s(float* z,
    const float* x, float s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = x[i] * s;
}

void simd_scalar_scale_add_s(float* z, float alpha,
    const float* x, float beta,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        z[i] = alpha * x[i] + beta;
}

void simd_scalar_sigmoid(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = fast_sigmoid(in[i]);
}

void simd_scalar_relu(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];

        out[i] = v > 0.0f ? v : (v == v ? 0.0f : v);
    }
}

void simd_scalar_relu6(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? (v > 6.0f ? 6.0f : v) : (v == v ? 0.0f : v);
    }
}

void simd_scalar_leaky_relu(float* out,
    const float* in, float slope,
    size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? v : v * slope;
    }
}

void simd_scalar_elu(float* out,
    const float* in, float alpha,
    size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
    }
}

void simd_scalar_tanh_fast(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = fast_tanh(in[i]);
}

void simd_scalar_gelu(float* out,
    const float* in, size_t n)
{
    const float sqrt_2_over_pi = 0.7978845608028654f;
    const float c = 0.044715f;
    for (size_t i = 0; i < n; i++) {
        float x = in[i], x3 = x * x * x;
        float inner = tanhf(sqrt_2_over_pi * (x + c * x3));
        out[i] = 0.5f * x * (1.0f + inner);
    }
}

void simd_scalar_silu(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = fast_silu(in[i]);
}

static void scalar_softmax_impl(float* out, const float* in, size_t n,
    int do_log)
{
    float maxv;
    if (n == 0)
        return;
    maxv = in[0];
    for (size_t i = 1; i < n; i++)
        if (in[i] > maxv)
            maxv = in[i];
    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += (double)fast_exp(in[i] - maxv);
    if (do_log) {
        float off = maxv + (float)log(sum);
        for (size_t i = 0; i < n; i++)
            out[i] = in[i] - off;
    } else {
        float inv_sum = (float)(1.0 / sum);
        for (size_t i = 0; i < n; i++)
            out[i] = fast_exp(in[i] - maxv) * inv_sum;
    }
}

void simd_scalar_softmax(float* out,
    const float* in, size_t n)
{
    scalar_softmax_impl(out, in, n, 0);
}

void simd_scalar_log_softmax(float* out,
    const float* in, size_t n)
{
    scalar_softmax_impl(out, in, n, 1);
}

void simd_scalar_vexp(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = fast_exp(in[i]);
}

void simd_scalar_vlog(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (v != v) {
            out[i] = v;
            continue;
        }
        out[i] = v > 0.0f ? logf(v) : -FLT_MAX;
    }
}

void simd_scalar_vsqrt(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (v != v) {
            out[i] = v;
            continue;
        }
        out[i] = v >= 0.0f ? sqrtf(v) : 0.0f;
    }
}

void simd_scalar_vrsqrt(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        if (v != v) {
            out[i] = v;
            continue;
        }
        out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
    }
}

void simd_scalar_vinv(float* out,
    const float* in, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

float simd_scalar_dist_l2_sq(const float* restrict a,
    const float* restrict b, size_t d)
{
    float acc = 0.0f;
    for (size_t i = 0; i < d; i++) {
        float df = a[i] - b[i];
        acc += df * df;
    }
    return acc;
}

float simd_scalar_dist_l1(const float* restrict a,
    const float* restrict b, size_t d)
{
    float acc = 0.0f;
    for (size_t i = 0; i < d; i++)
        acc += fabsf(a[i] - b[i]);
    return acc;
}

float simd_scalar_dist_cos(const float* restrict a,
    const float* restrict b, size_t d)
{
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < d; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    double denom = sqrt(na * nb);
    if (denom < (double)FLT_MIN)
        return 1.0f;
    return (float)(1.0 - dot / denom);
}

float simd_scalar_dist_cheb(const float* restrict a,
    const float* restrict b, size_t d)
{
    float maxv = 0.0f;
    for (size_t i = 0; i < d; i++) {
        float df = fabsf(a[i] - b[i]);
        if (df > maxv || df != df)
            maxv = df;
    }
    return maxv;
}

float simd_scalar_dist_l2(const float* restrict a,
    const float* restrict b, size_t d)
{
    return sqrtf(simd_scalar_dist_l2_sq(a, b, d));
}

void simd_scalar_dist_matrix_l2_sq(float* restrict out,
    const float* restrict a,
    const float* restrict b, size_t n,
    size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_scalar_dist_l2_sq(&a[i * d], &b[j * d], d);
}

void simd_scalar_dist_matrix_cos(float* restrict out,
    const float* restrict a,
    const float* restrict b, size_t n,
    size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_scalar_dist_cos(&a[i * d], &b[j * d], d);
}

void simd_scalar_dist_matrix_l1(float* restrict out,
    const float* restrict a,
    const float* restrict b, size_t n,
    size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_scalar_dist_l1(&a[i * d], &b[j * d], d);
}

void simd_scalar_gemv(float* restrict y, const float* restrict a,
    const float* restrict x, size_t m, size_t n,
    float beta)
{
    for (size_t i = 0; i < m; i++) {
        double acc = 0.0;
        for (size_t j = 0; j < n; j++)
            acc += (double)a[i * n + j] * (double)x[j];
        y[i] = beta == 0.0f ? (float)acc : beta * y[i] + (float)acc;
    }
}

void simd_scalar_gemv_t(float* restrict y,
    const float* restrict a,
    const float* restrict x, size_t m, size_t n,
    float beta)
{
    if (beta == 0.0f) {
        for (size_t j = 0; j < n; j++)
            y[j] = 0.0f;
    } else {
        for (size_t j = 0; j < n; j++)
            y[j] *= beta;
    }
    for (size_t i = 0; i < m; i++) {
        float xi = x[i];
        const float* row = &a[i * n];
        for (size_t j = 0; j < n; j++)
            y[j] += xi * row[j];
    }
}

void simd_scalar_gemm(float* restrict c, const float* restrict a,
    const float* restrict b, size_t m, size_t n,
    size_t k, float alpha, float beta)
{
    const size_t T = 32;
    if (beta == 0.0f) {
        for (size_t i = 0; i < m; i++)
            for (size_t j = 0; j < n; j++)
                c[i * n + j] = 0.0f;
    } else {
        for (size_t i = 0; i < m; i++)
            for (size_t j = 0; j < n; j++)
                c[i * n + j] *= beta;
    }

    for (size_t i0 = 0; i0 < m; i0 += T) {
        size_t imax = i0 + T < m ? i0 + T : m;
        for (size_t j0 = 0; j0 < n; j0 += T) {
            size_t jmax = j0 + T < n ? j0 + T : n;
            for (size_t k0 = 0; k0 < k; k0 += T) {
                size_t kmax = k0 + T < k ? k0 + T : k;
                for (size_t i = i0; i < imax; i++) {
                    for (size_t kk = k0; kk < kmax; kk++) {
                        float aik = alpha * a[i * k + kk];
                        for (size_t j = j0; j < jmax; j++)
                            c[i * n + j] += aik * b[kk * n + j];
                    }
                }
            }
        }
    }
}

void simd_scalar_threshold(float* out,
    const float* in, float t,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = in[i] > t ? 1.0f : 0.0f;
}

void simd_scalar_threshold_sign(float* restrict out,
    const float* restrict in, float t,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = in[i] >= t ? 1.0f : -1.0f;
}

float simd_scalar_hamming(const uint32_t* restrict a,
    const uint32_t* restrict b,
    size_t n_words)
{
    uint32_t diff = 0;
    for (size_t i = 0; i < n_words; i++)
        diff += (uint32_t)dyn_popcount32(a[i] ^ b[i]);
    return (float)diff;
}

float simd_scalar_sigmoid_f(float x) { return fast_sigmoid(x); }
float simd_scalar_tanh_f(float x) { return fast_tanh(x); }
float simd_scalar_exp_f(float x) { return fast_exp(x); }

static inline float topk_key(float v)
{
    return v != v ? -INFINITY : v;
}

static inline void heap_sift_down(uint32_t* heap, const float* vals,
    size_t k, size_t idx)
{
    float key = topk_key(vals[heap[idx]]);
    while (1) {
        size_t left = 2 * idx + 1;
        if (left >= k)
            break;
        size_t right = left + 1;
        size_t smaller = (right < k && topk_key(vals[heap[right]]) < topk_key(vals[heap[left]])) ? right : left;
        if (key <= topk_key(vals[heap[smaller]]))
            break;
        uint32_t tmp = heap[idx];
        heap[idx] = heap[smaller];
        heap[smaller] = tmp;
        idx = smaller;
    }
}

void simd_scalar_topk_indices(const float* restrict vals,
    uint32_t* restrict indices, size_t n,
    size_t k)
{
    if (k == 0 || n == 0)
        return;
    if (k > n)
        k = n;
    for (size_t i = 0; i < k; i++)
        indices[i] = (uint32_t)i;
    for (size_t i = k / 2; i > 0; i--)
        heap_sift_down(indices, vals, k, i - 1);
    for (size_t i = k; i < n; i++) {
        if (topk_key(vals[i]) > topk_key(vals[indices[0]])) {
            indices[0] = (uint32_t)i;
            heap_sift_down(indices, vals, k, 0);
        }
    }
}

void simd_scalar_clamp(float* out,
    const float* in, float lo, float hi,
    size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i];
        out[i] = v < lo ? lo : (v > hi ? hi : v);
    }
}

double simd_scalar_f64_sum(const double* restrict x, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += x[i];
    return acc;
}

double simd_scalar_f64_dot(const double* restrict a, const double* restrict b,
    size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += a[i] * b[i];
    return acc;
}

double simd_scalar_f64_min(const double* restrict x, size_t n)
{
    double m = n ? (double)INFINITY : DBL_MAX;
    for (size_t i = 0; i < n; i++)
        if (x[i] < m)
            m = x[i];
    return m;
}

double simd_scalar_f64_max(const double* restrict x, size_t n)
{
    double m = n ? -(double)INFINITY : -DBL_MAX;
    for (size_t i = 0; i < n; i++)
        if (x[i] > m)
            m = x[i];
    return m;
}

void simd_scalar_f64_scale(double* out, const double* x,
    double s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = x[i] * s;
}

void simd_scalar_f64_axpy(double* restrict y, double a,
    const double* restrict x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        double p = a * x[i];
        y[i] = y[i] + p;
    }
}

int64_t simd_scalar_i32_sum(const int32_t* restrict x, size_t n)
{
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc += (int64_t)x[i];
    return acc;
}

int32_t simd_scalar_i32_min(const int32_t* restrict x, size_t n)
{
    int32_t m = INT32_MAX;
    for (size_t i = 0; i < n; i++)
        if (x[i] < m)
            m = x[i];
    return m;
}

int32_t simd_scalar_i32_max(const int32_t* restrict x, size_t n)
{
    int32_t m = INT32_MIN;
    for (size_t i = 0; i < n; i++)
        if (x[i] > m)
            m = x[i];
    return m;
}

double simd_scalar_i32_dot(const int32_t* restrict a, const int32_t* restrict b,
    size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += (double)a[i] * (double)b[i];
    return acc;
}

void simd_scalar_i32_add(int32_t* restrict out, const int32_t* restrict a,
    const int32_t* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

void simd_scalar_i32_mul(int32_t* restrict out, const int32_t* restrict a,
    const int32_t* restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

void simd_scalar_i32_scale(int32_t* out, const int32_t* x,
    int32_t s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}

void simd_scalar_f32_cumsum(float* out, const float* x, size_t n)
{
    float acc = 0.0f;
    for (size_t i = 0; i < n; i++) {
        acc += x[i];
        out[i] = acc;
    }
}

void simd_scalar_i32_cumsum(int32_t* out, const int32_t* x, size_t n)
{
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (uint32_t)x[i];
        out[i] = (int32_t)acc;
    }
}

void simd_scalar_f32_cummax(float* out, const float* x, size_t n)
{
    float acc = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        if (x[i] > acc)
            acc = x[i];
        out[i] = acc;
    }
}

void simd_scalar_i32_cummax(int32_t* out, const int32_t* x, size_t n)
{
    int32_t acc = INT32_MIN;
    for (size_t i = 0; i < n; i++) {
        if (x[i] > acc)
            acc = x[i];
        out[i] = acc;
    }
}

static size_t simd_scalar_find_u8(const uint8_t* restrict p, uint8_t v,
    size_t n)
{
    const void* r = memchr(p, v, n);
    return r ? (size_t)((const uint8_t*)r - p) : SIZE_MAX;
}
static size_t simd_scalar_find_u16(const uint16_t* restrict p, uint16_t v,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            return i;
    return SIZE_MAX;
}
static size_t simd_scalar_find_u32(const uint32_t* restrict p, uint32_t v,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            return i;
    return SIZE_MAX;
}
static size_t simd_scalar_find_f32(const float* restrict p, float v,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            return i;
    return SIZE_MAX;
}
static size_t simd_scalar_find_f64(const double* restrict p, double v,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            return i;
    return SIZE_MAX;
}

static size_t simd_scalar_strfind(const uint8_t* text, size_t n,
    const uint8_t* pat, size_t m)
{
    if (m == 0)
        return 0;
    if (m > n)
        return SIZE_MAX;
    if (m == 1) {
        const void* r = memchr(text, pat[0], n);
        return r ? (size_t)((const uint8_t*)r - text) : SIZE_MAX;
    }
    size_t limit = n - m;
    size_t i = 0;
    size_t fails = 0;
    while (i <= limit) {
        const uint8_t* hit = memchr(text + i, pat[0], limit - i + 1);
        if (!hit)
            break;
        size_t pos = (size_t)(hit - text);
        if (memcmp(text + pos + 1, pat + 1, m - 1) == 0)
            return pos;
        i = pos + 1;
        if (simd_strfind_fail(&fails, pos, m))
            return simd_strfind_resume(text, n, pat, m, i);
    }
    return SIZE_MAX;
}

static size_t simd_scalar_count_u8(const uint8_t* restrict p, uint8_t v,
    size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            c++;
    return c;
}

static size_t simd_scalar_find_first_of_u16(const uint16_t* restrict p,
    size_t n,
    const uint16_t* restrict set,
    size_t setlen)
{
    if (setlen == 0)
        return SIZE_MAX;
    for (size_t i = 0; i < n; i++)
        for (size_t k = 0; k < setlen; k++)
            if (p[i] == set[k])
                return i;
    return SIZE_MAX;
}

static size_t simd_scalar_find_first_of(const uint8_t* restrict p, size_t n,
    const uint8_t* restrict set,
    size_t setlen)
{
    uint8_t tbl[256];
    if (setlen == 0)
        return SIZE_MAX;
    memset(tbl, 0, sizeof(tbl));
    for (size_t i = 0; i < setlen; i++)
        tbl[set[i]] = 1;
    for (size_t i = 0; i < n; i++)
        if (tbl[p[i]])
            return i;
    return SIZE_MAX;
}

static size_t simd_scalar_validate_utf8(const uint8_t* restrict p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else
            return i;
        if (i + len > n)
            return i;
        for (size_t j = 1; j < len; j++) {
            uint8_t cc = p[i + j];
            if ((cc & 0xC0) != 0x80)
                return i;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (len == 2 && cp < 0x80)
            return i;
        if (len == 3 && cp < 0x800)
            return i;
        if (len == 4 && cp < 0x10000)
            return i;
        if (cp > 0x10FFFF)
            return i;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return i;
        i += len;
    }
    return n;
}

static const char simd_b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t simd_scalar_base64_encode(const uint8_t* restrict src, size_t n,
    char* restrict dst)
{
    const char* E = simd_b64_alphabet;
    size_t i = 0, o = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t x = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = E[(x >> 18) & 0x3F];
        dst[o++] = E[(x >> 12) & 0x3F];
        dst[o++] = E[(x >> 6) & 0x3F];
        dst[o++] = E[x & 0x3F];
    }
    if (n - i == 1) {
        uint32_t x = (uint32_t)src[i] << 16;
        dst[o++] = E[(x >> 18) & 0x3F];
        dst[o++] = E[(x >> 12) & 0x3F];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (n - i == 2) {
        uint32_t x = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = E[(x >> 18) & 0x3F];
        dst[o++] = E[(x >> 12) & 0x3F];
        dst[o++] = E[(x >> 6) & 0x3F];
        dst[o++] = '=';
    }
    return o;
}

static const int8_t simd_b64_dec[256] = {
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    62,
    -1,
    -1,
    -1,
    63,
    52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
};

static size_t simd_scalar_base64_decode(const char* restrict src, size_t n,
    uint8_t* restrict dst)
{
    const int8_t* dec = simd_b64_dec;
    size_t i, o = 0;
    if (n % 4 != 0)
        return SIZE_MAX;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i += 4) {
        int last = (i + 4 == n);
        uint8_t c0 = (uint8_t)src[i], c1 = (uint8_t)src[i + 1];
        uint8_t c2 = (uint8_t)src[i + 2], c3 = (uint8_t)src[i + 3];
        int v0 = dec[c0], v1 = dec[c1], v2, v3, pad = 0;
        uint32_t x;
        if (v0 < 0 || v1 < 0)
            return SIZE_MAX;
        if (c2 == '=') {
            if (!last || c3 != '=')
                return SIZE_MAX;
            pad = 2;
            v2 = 0;
            v3 = 0;
        } else {
            v2 = dec[c2];
            if (v2 < 0)
                return SIZE_MAX;
            if (c3 == '=') {
                if (!last)
                    return SIZE_MAX;
                pad = 1;
                v3 = 0;
            } else {
                v3 = dec[c3];
                if (v3 < 0)
                    return SIZE_MAX;
            }
        }
        x = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        dst[o++] = (x >> 16) & 0xFF;
        if (pad < 2)
            dst[o++] = (x >> 8) & 0xFF;
        if (pad < 1)
            dst[o++] = x & 0xFF;
    }
    return o;
}

static const char simd_hex_lut[] = "0123456789abcdef";

static inline int simd_hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void simd_scalar_hex_encode(const uint8_t* restrict src, size_t n,
    char* restrict dst)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        dst[o++] = simd_hex_lut[src[i] >> 4];
        dst[o++] = simd_hex_lut[src[i] & 0x0F];
    }
}

static size_t simd_scalar_hex_decode(const char* restrict src, size_t n,
    uint8_t* restrict dst)
{
    size_t i, o = 0;
    if (n & 1)
        return SIZE_MAX;
    for (i = 0; i < n; i += 2) {
        int hi = simd_hex_val((uint8_t)src[i]);
        int lo = simd_hex_val((uint8_t)src[i + 1]);
        if (hi < 0 || lo < 0)
            return SIZE_MAX;
        dst[o++] = (uint8_t)((hi << 4) | lo);
    }
    return o;
}

static size_t simd_scalar_latin1_to_utf8(const uint8_t* restrict src, size_t n,
    uint8_t* restrict dst)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        uint8_t c = src[i];
        if (c < 0x80) {
            dst[o++] = c;
        } else {
            dst[o++] = (uint8_t)(0xC0 | (c >> 6));
            dst[o++] = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

static int simd_scalar_utf8_to_latin1(const uint8_t* restrict src, size_t n,
    uint8_t* restrict dst, size_t* out_len)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = src[i];
        if (c < 0x80) {
            dst[o++] = c;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            uint8_t c1;
            uint32_t cp;
            if (i + 1 >= n)
                return -1;
            c1 = src[i + 1];
            if ((c1 & 0xC0) != 0x80)
                return -1;
            cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
            if (cp < 0x80 || cp > 0xFF)
                return -1;
            dst[o++] = (uint8_t)cp;
            i += 2;
            continue;
        }
        return -1;
    }
    *out_len = o;
    return 0;
}

static size_t simd_scalar_count_utf8(const uint8_t* restrict p, size_t n)
{
    size_t i, c = 0;
    for (i = 0; i < n; i++)
        if ((p[i] & 0xC0) != 0x80)
            c++;
    return c;
}

static int simd_scalar_utf8_to_utf16le(const uint8_t* restrict src, size_t n,
    uint16_t* restrict dst,
    size_t* out_units)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = src[i];
        size_t len, j;
        uint32_t cp;
        if (c < 0x80) {
            dst[o++] = c;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else
            return -1;
        if (i + len > n)
            return -1;
        for (j = 1; j < len; j++) {
            uint8_t cc = src[i + j];
            if ((cc & 0xC0) != 0x80)
                return -1;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (len == 2 && cp < 0x80)
            return -1;
        if (len == 3 && cp < 0x800)
            return -1;
        if (len == 4 && cp < 0x10000)
            return -1;
        if (cp > 0x10FFFF)
            return -1;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return -1;
        if (cp < 0x10000) {
            dst[o++] = (uint16_t)cp;
        } else {
            cp -= 0x10000;
            dst[o++] = (uint16_t)(0xD800 | (cp >> 10));
            dst[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        }
        i += len;
    }
    *out_units = o;
    return 0;
}

static int simd_scalar_utf16le_to_utf8(const uint16_t* restrict src,
    size_t units, uint8_t* restrict dst,
    size_t* out_len)
{
    size_t i = 0, o = 0;
    while (i < units) {
        uint32_t cp = src[i];
        if (cp < 0xD800 || cp > 0xDFFF) {
            i++;
        } else if (cp <= 0xDBFF) {
            uint32_t lo;
            if (i + 1 >= units)
                return -1;
            lo = src[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF)
                return -1;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else {
            return -1;
        }
        if (cp < 0x80) {
            dst[o++] = (uint8_t)cp;
        } else if (cp < 0x800) {
            dst[o++] = (uint8_t)(0xC0 | (cp >> 6));
            dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            dst[o++] = (uint8_t)(0xE0 | (cp >> 12));
            dst[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        } else {
            dst[o++] = (uint8_t)(0xF0 | (cp >> 18));
            dst[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        }
    }
    *out_len = o;
    return 0;
}

static int simd_scalar_validate_utf16le(const uint16_t* restrict src,
    size_t units)
{
    size_t i = 0;
    while (i < units) {
        uint32_t c = src[i];
        if (c < 0xD800 || c > 0xDFFF) {
            i++;
            continue;
        }
        if (c > 0xDBFF)
            return 0;
        if (i + 1 >= units)
            return 0;
        if (src[i + 1] < 0xDC00 || src[i + 1] > 0xDFFF)
            return 0;
        i += 2;
    }
    return 1;
}

static size_t simd_scalar_count_utf16(const uint16_t* restrict src,
    size_t units)
{
    size_t i, c = 0;
    for (i = 0; i < units; i++)
        if (src[i] < 0xDC00 || src[i] > 0xDFFF)
            c++;
    return c;
}

static size_t simd_scalar_find_bitmap(const uint8_t* restrict p, size_t n,
    const uint8_t* restrict bitmap)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (bitmap[p[i] >> 3] & (uint8_t)(1u << (p[i] & 7)))
            return i;
    return SIZE_MAX;
}

void simd_override_scalar(simd_t* t)
{
    t->find_u8 = simd_scalar_find_u8;
    t->find_u16 = simd_scalar_find_u16;
    t->find_u32 = simd_scalar_find_u32;
    t->find_f32 = simd_scalar_find_f32;
    t->find_f64 = simd_scalar_find_f64;
    t->strfind = simd_scalar_strfind;
    t->count_u8 = simd_scalar_count_u8;
    t->find_first_of = simd_scalar_find_first_of;
    t->find_first_of_u16 = simd_scalar_find_first_of_u16;
    t->find_bitmap = simd_scalar_find_bitmap;
    t->validate_utf8 = simd_scalar_validate_utf8;
    t->base64_encode = simd_scalar_base64_encode;
    t->base64_decode = simd_scalar_base64_decode;
    t->hex_encode = simd_scalar_hex_encode;
    t->hex_decode = simd_scalar_hex_decode;
    t->latin1_to_utf8 = simd_scalar_latin1_to_utf8;
    t->utf8_to_latin1 = simd_scalar_utf8_to_latin1;
    t->count_utf8 = simd_scalar_count_utf8;
    t->utf8_to_utf16le = simd_scalar_utf8_to_utf16le;
    t->utf16le_to_utf8 = simd_scalar_utf16le_to_utf8;
    t->validate_utf16le = simd_scalar_validate_utf16le;
    t->count_utf16 = simd_scalar_count_utf16;
    t->dot = simd_scalar_dot;
    t->dot_f = simd_scalar_dot_f;
    t->norm_l2_sq = simd_scalar_norm_l2_sq;
    t->norm_l2 = simd_scalar_norm_l2;
    t->norm_l1 = simd_scalar_norm_l1;
    t->axpy = simd_scalar_axpy;
    t->axpby = simd_scalar_axpby;
    t->sum = simd_scalar_sum;
    t->max = simd_scalar_max;
    t->min = simd_scalar_min;
    t->argmax = simd_scalar_argmax;
    t->argmin = simd_scalar_argmin;
    t->argminmax = simd_scalar_argminmax;
    t->add = simd_scalar_add;
    t->sub = simd_scalar_sub;
    t->mul = simd_scalar_mul;
    t->div = simd_scalar_div;
    t->abs = simd_scalar_abs;
    t->fma = simd_scalar_fma;
    t->add_s = simd_scalar_add_s;
    t->mul_s = simd_scalar_mul_s;
    t->scale_add_s = simd_scalar_scale_add_s;
    t->sigmoid = simd_scalar_sigmoid;
    t->relu = simd_scalar_relu;
    t->relu6 = simd_scalar_relu6;
    t->leaky_relu = simd_scalar_leaky_relu;
    t->elu = simd_scalar_elu;
    t->tanh_fast = simd_scalar_tanh_fast;
    t->gelu = simd_scalar_gelu;
    t->silu = simd_scalar_silu;
    t->softmax = simd_scalar_softmax;
    t->log_softmax = simd_scalar_log_softmax;
    t->vexp = simd_scalar_vexp;
    t->vlog = simd_scalar_vlog;
    t->vsqrt = simd_scalar_vsqrt;
    t->vrsqrt = simd_scalar_vrsqrt;
    t->vinv = simd_scalar_vinv;
    t->dist_l2_sq = simd_scalar_dist_l2_sq;
    t->dist_l2 = simd_scalar_dist_l2;
    t->dist_l1 = simd_scalar_dist_l1;
    t->dist_cos = simd_scalar_dist_cos;
    t->dist_cheb = simd_scalar_dist_cheb;
    t->dist_matrix_l2_sq = simd_scalar_dist_matrix_l2_sq;
    t->dist_matrix_cos = simd_scalar_dist_matrix_cos;
    t->dist_matrix_l1 = simd_scalar_dist_matrix_l1;
    t->gemv = simd_scalar_gemv;
    t->gemv_t = simd_scalar_gemv_t;
    t->gemm = simd_scalar_gemm;
    t->threshold = simd_scalar_threshold;
    t->threshold_sign = simd_scalar_threshold_sign;
    t->argminmax = simd_scalar_argminmax;
    t->hamming = simd_scalar_hamming;
    t->sigmoid_f = simd_scalar_sigmoid_f;
    t->tanh_f = simd_scalar_tanh_f;
    t->exp_f = simd_scalar_exp_f;
    t->topk_indices = simd_scalar_topk_indices;
    t->clamp = simd_scalar_clamp;
    t->f64_sum = simd_scalar_f64_sum;
    t->f64_dot = simd_scalar_f64_dot;
    t->f64_min = simd_scalar_f64_min;
    t->f64_max = simd_scalar_f64_max;
    t->f64_scale = simd_scalar_f64_scale;
    t->f64_axpy = simd_scalar_f64_axpy;
    t->i32_sum = simd_scalar_i32_sum;
    t->i32_min = simd_scalar_i32_min;
    t->i32_max = simd_scalar_i32_max;
    t->i32_dot = simd_scalar_i32_dot;
    t->i32_add = simd_scalar_i32_add;
    t->i32_mul = simd_scalar_i32_mul;
    t->i32_scale = simd_scalar_i32_scale;
    t->f32_cumsum = simd_scalar_f32_cumsum;
    t->i32_cumsum = simd_scalar_i32_cumsum;
    t->f32_cummax = simd_scalar_f32_cummax;
    t->i32_cummax = simd_scalar_i32_cummax;
}