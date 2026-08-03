/*
 * Copyright 2026 Piyush Katariya
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Scalar (non-SIMD) fallback for every ML kernel.
 * Always compiled; used when no ISA override is active.
 * The compiler may auto-vectorize at -O3; hand-coded SIMD overrides
 * in simd_{avx2,avx512,neon,sve}.c take precedence via dispatch. */

#include <math.h>
#include "dyna-simd-kernels.h"
#include <string.h>
#include <float.h>
#include <math.h>

/* ── BLAS Level 1: dot products ──────────────────────────────────── */

float simd_scalar_dot_f(const float *restrict a,
                               const float *restrict b, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += (double)a[i] * (double)b[i];
  return (float)acc;
}

float simd_scalar_dot(const float *restrict a,
                             const float *restrict b, size_t n) {
  float acc = 0.0f;
  for (size_t i = 0; i < n; i++)
    acc += a[i] * b[i];
  return acc;
}

float simd_scalar_norm_l2_sq(const float *restrict x, size_t n) {
  float acc = 0.0f;
  for (size_t i = 0; i < n; i++)
    acc += x[i] * x[i];
  return acc;
}

float simd_scalar_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_scalar_norm_l2_sq(x, n));
}

float simd_scalar_norm_l1(const float *restrict x, size_t n) {
  float acc = 0.0f;
  for (size_t i = 0; i < n; i++)
    acc += fabsf(x[i]);
  return acc;
}

void simd_scalar_axpy(float *restrict y, float alpha,
                             const float *restrict x, size_t n) {
  for (size_t i = 0; i < n; i++)
    y[i] += alpha * x[i];
}

void simd_scalar_axpby(float *restrict z, float a,
                              const float *restrict x, float b,
                              const float *restrict y, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = a * x[i] + b * y[i];
}

/* ── Reductions ─────────────────────────────────────────────────── */

float simd_scalar_sum(const float *restrict x, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += (double)x[i];
  return (float)acc;
}

float simd_scalar_max(const float *restrict x, size_t n) {
  float m = -FLT_MAX;
  for (size_t i = 0; i < n; i++)
    if (x[i] > m)
      m = x[i];
  return m;
}

float simd_scalar_min(const float *restrict x, size_t n) {
  float m = FLT_MAX;
  for (size_t i = 0; i < n; i++)
    if (x[i] < m)
      m = x[i];
  return m;
}

size_t simd_scalar_argmax(const float *restrict x, size_t n) {
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

size_t simd_scalar_argmin(const float *restrict x, size_t n) {
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

void simd_scalar_argminmax(const float *restrict x, size_t n,
                                  size_t *argmin_out, size_t *argmax_out) {
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

/* ── Element-wise: vector–vector ─────────────────────────────────── */

void simd_scalar_add(float *restrict z, const float *restrict a,
                            const float *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = a[i] + b[i];
}

void simd_scalar_sub(float *restrict z, const float *restrict a,
                            const float *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = a[i] - b[i];
}

void simd_scalar_mul(float *restrict z, const float *restrict a,
                            const float *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = a[i] * b[i];
}

void simd_scalar_div(float *restrict z, const float *restrict a,
                            const float *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = a[i] / b[i];
}

void simd_scalar_abs(float *restrict out,
                            const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = fabsf(in[i]);
}

void simd_scalar_fma(float *restrict z, const float *restrict a,
                            const float *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ─────────────────────────────────── */

void simd_scalar_add_s(float *restrict z,
                              const float *restrict x, float s, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = x[i] + s;
}

void simd_scalar_mul_s(float *restrict z,
                              const float *restrict x, float s, size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = x[i] * s;
}

void simd_scalar_scale_add_s(float *restrict z, float alpha,
                                    const float *restrict x, float beta,
                                    size_t n) {
  for (size_t i = 0; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ─────────────────────────────────────────────────── */

void simd_scalar_sigmoid(float *restrict out,
                                const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

void simd_scalar_relu(float *restrict out,
                             const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

void simd_scalar_relu6(float *restrict out,
                              const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

void simd_scalar_leaky_relu(float *restrict out,
                                   const float *restrict in, float slope,
                                   size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

void simd_scalar_elu(float *restrict out,
                            const float *restrict in, float alpha,
                            size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

void simd_scalar_tanh_fast(float *restrict out,
                                  const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

void simd_scalar_gelu(float *restrict out,
                             const float *restrict in, size_t n) {
  const float sqrt_2_over_pi = 0.7978845608028654f;
  const float c = 0.044715f;
  for (size_t i = 0; i < n; i++) {
    float x = in[i], x3 = x * x * x;
    float inner = tanhf(sqrt_2_over_pi * (x + c * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

void simd_scalar_silu(float *restrict out,
                             const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v / (1.0f + expf(-v));
  }
}

/* ── Softmax family ──────────────────────────────────────────────── */

static void scalar_softmax_impl(float *restrict out,
                                       const float *restrict in, size_t n,
                                       int do_log) {
  float maxv = in[0];
  for (size_t i = 1; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    float e = fast_exp(in[i] - maxv);
    out[i] = e;
    sum += (double)e;
  }
  float inv_sum = (float)(1.0 / sum);
  if (do_log) {
    float log_inv = logf(inv_sum);
    for (size_t i = 0; i < n; i++)
      out[i] = logf(out[i]) + log_inv;
  } else {
    for (size_t i = 0; i < n; i++)
      out[i] *= inv_sum;
  }
}

void simd_scalar_softmax(float *restrict out,
                                const float *restrict in, size_t n) {
  scalar_softmax_impl(out, in, n, 0);
}

void simd_scalar_log_softmax(float *restrict out,
                                    const float *restrict in, size_t n) {
  scalar_softmax_impl(out, in, n, 1);
}

/* ── Element-wise unary math ─────────────────────────────────────── */

void simd_scalar_vexp(float *restrict out,
                             const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = fast_exp(in[i]);
}

void simd_scalar_vlog(float *restrict out,
                             const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

void simd_scalar_vsqrt(float *restrict out,
                              const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

void simd_scalar_vrsqrt(float *restrict out,
                               const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

void simd_scalar_vinv(float *restrict out,
                             const float *restrict in, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────────── */

float simd_scalar_dist_l2_sq(const float *restrict a,
                                    const float *restrict b, size_t d) {
  float acc = 0.0f;
  for (size_t i = 0; i < d; i++) {
    float df = a[i] - b[i];
    acc += df * df;
  }
  return acc;
}

float simd_scalar_dist_l1(const float *restrict a,
                                 const float *restrict b, size_t d) {
  float acc = 0.0f;
  for (size_t i = 0; i < d; i++)
    acc += fabsf(a[i] - b[i]);
  return acc;
}

float simd_scalar_dist_cos(const float *restrict a,
                                  const float *restrict b, size_t d) {
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < d; i++) {
    dot += (double)a[i] * b[i];
    na += (double)a[i] * a[i];
    nb += (double)b[i] * b[i];
  }
  double denom = sqrt(na * nb);
  if (denom < FLT_MIN)
    return 1.0f;
  return (float)(1.0 - dot / denom);
}

float simd_scalar_dist_cheb(const float *restrict a,
                                   const float *restrict b, size_t d) {
  float maxv = 0.0f;
  for (size_t i = 0; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > maxv)
      maxv = df;
  }
  return maxv;
}

float simd_scalar_dist_l2(const float *restrict a,
                                 const float *restrict b, size_t d) {
  return sqrtf(simd_scalar_dist_l2_sq(a, b, d));
}

/* ── Distance matrix ─────────────────────────────────────────────── */

void simd_scalar_dist_matrix_l2_sq(float *restrict out,
                                          const float *restrict a,
                                          const float *restrict b, size_t n,
                                          size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_scalar_dist_l2_sq(&a[i * d], &b[j * d], d);
}

void simd_scalar_dist_matrix_cos(float *restrict out,
                                        const float *restrict a,
                                        const float *restrict b, size_t n,
                                        size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_scalar_dist_cos(&a[i * d], &b[j * d], d);
}

void simd_scalar_dist_matrix_l1(float *restrict out,
                                       const float *restrict a,
                                       const float *restrict b, size_t n,
                                       size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_scalar_dist_l1(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: matrix–vector ─────────────────────────────────── */

void simd_scalar_gemv(float *restrict y, const float *restrict a,
                             const float *restrict x, size_t m, size_t n,
                             float beta) {
  for (size_t i = 0; i < m; i++) {
    double acc = 0.0;
    for (size_t j = 0; j < n; j++)
      acc += (double)a[i * n + j] * x[j];
    y[i] = beta * y[i] + (float)acc;
  }
}

void simd_scalar_gemv_t(float *restrict y,
                               const float *restrict a,
                               const float *restrict x, size_t m, size_t n,
                               float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    float xi = x[i];
    const float *row = &a[i * n];
    for (size_t j = 0; j < n; j++)
      y[j] += xi * row[j];
  }
}

/* ── BLAS Level 3: matrix–matrix (cache-tiled) ──────────────────── */

void simd_scalar_gemm(float *restrict c, const float *restrict a,
                             const float *restrict b, size_t m, size_t n,
                             size_t k, float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j < n; j++)
      c[i * n + j] *= beta;

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

/* ── Comparison / Selection ──────────────────────────────────────── */

void simd_scalar_threshold(float *restrict out,
                                  const float *restrict in, float t,
                                  size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

void simd_scalar_threshold_sign(float *restrict out,
                                       const float *restrict in, float t,
                                       size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ─────────────────────────────────────────────────────── */

float simd_scalar_hamming(const uint32_t *restrict a,
                                 const uint32_t *restrict b,
                                 size_t n_words) {
  uint32_t diff = 0;
  for (size_t i = 0; i < n_words; i++)
    diff += (uint32_t)__builtin_popcount(a[i] ^ b[i]);
  return (float)diff;
}

/* ── Scalar fast math ────────────────────────────────────────────── */

float simd_scalar_sigmoid_f(float x) { return fast_sigmoid(x); }
float simd_scalar_tanh_f(float x) { return fast_tanh(x); }
float simd_scalar_exp_f(float x) { return fast_exp(x); }

/* ── Top-K via min-heap (keep k largest) ────────────────────────── */

/* MIN-heap sift-down: the root is the SMALLEST of the kept set. topk_indices
 * keeps the k LARGEST by evicting the current smallest (the root) whenever a
 * larger value arrives, so the heap must order on the minimum. */
static inline void heap_sift_down(uint32_t *heap, const float *vals,
                                      size_t k, size_t idx) {
  float key = vals[heap[idx]];
  while (1) {
    size_t left = 2 * idx + 1;
    if (left >= k)
      break;
    size_t right = left + 1;
    size_t smaller =
        (right < k && vals[heap[right]] < vals[heap[left]]) ? right : left;
    if (key <= vals[heap[smaller]])
      break;
    uint32_t tmp = heap[idx];
    heap[idx] = heap[smaller];
    heap[smaller] = tmp;
    idx = smaller;
  }
}

void simd_scalar_topk_indices(const float *restrict vals,
                                     uint32_t *restrict indices, size_t n,
                                     size_t k) {
  if (k == 0 || n == 0)
    return;
  if (k > n)
    k = n;
  for (size_t i = 0; i < k; i++)
    indices[i] = (uint32_t)i;
  for (size_t i = k / 2; i > 0; i--)
    heap_sift_down(indices, vals, k, i - 1);
  for (size_t i = k; i < n; i++) {
    /* root is the smallest kept; a larger value evicts it (keep k largest) */
    if (vals[i] > vals[indices[0]]) {
      indices[0] = (uint32_t)i;
      heap_sift_down(indices, vals, k, 0);
    }
  }
}

/* ── Clamp ───────────────────────────────────────────────────────── */

void simd_scalar_clamp(float *restrict out,
                              const float *restrict in, float lo, float hi,
                              size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* ── Double-precision (f64) array kernels ────────────────────────── */

double simd_scalar_f64_sum(const double *restrict x, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += x[i];
  return acc;
}

double simd_scalar_f64_dot(const double *restrict a, const double *restrict b,
                           size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += a[i] * b[i];
  return acc;
}

double simd_scalar_f64_min(const double *restrict x, size_t n) {
  double m = DBL_MAX;
  for (size_t i = 0; i < n; i++)
    if (x[i] < m)
      m = x[i];
  return m;
}

double simd_scalar_f64_max(const double *restrict x, size_t n) {
  double m = -DBL_MAX;
  for (size_t i = 0; i < n; i++)
    if (x[i] > m)
      m = x[i];
  return m;
}

void simd_scalar_f64_scale(double *restrict out, const double *restrict x,
                           double s, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = x[i] * s;
}

/* y[i] += a*x[i], as a NON-fused multiply-then-add so every ISA (and JS) agree
 * bit-for-bit. The split into two statements blocks FMA contraction. */
void simd_scalar_f64_axpy(double *restrict y, double a,
                          const double *restrict x, size_t n) {
  for (size_t i = 0; i < n; i++) {
    double p = a * x[i];
    y[i] = y[i] + p;
  }
}

/* ── Signed 32-bit integer (i32) array kernels ───────────────────── */

/* Exact int64 accumulation: cannot overflow for any realistic length, and is
 * the differential oracle every SIMD i32_sum must equal bit-for-bit. */
int64_t simd_scalar_i32_sum(const int32_t *restrict x, size_t n) {
  int64_t acc = 0;
  for (size_t i = 0; i < n; i++)
    acc += (int64_t)x[i];
  return acc;
}

int32_t simd_scalar_i32_min(const int32_t *restrict x, size_t n) {
  int32_t m = INT32_MAX;
  for (size_t i = 0; i < n; i++)
    if (x[i] < m)
      m = x[i];
  return m;
}

int32_t simd_scalar_i32_max(const int32_t *restrict x, size_t n) {
  int32_t m = INT32_MIN;
  for (size_t i = 0; i < n; i++)
    if (x[i] > m)
      m = x[i];
  return m;
}

/* Sum of int64 products in a double (returns a JS Number). (double)a*(double)b
 * is the correctly-rounded product, identical to (double)((int64_t)a*b). */
double simd_scalar_i32_dot(const int32_t *restrict a, const int32_t *restrict b,
                           size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    acc += (double)a[i] * (double)b[i];
  return acc;
}

/* out[i] = a[i] + b[i], wrapping mod 2^32 like JS `(a+b)|0`. Done through
 * unsigned to avoid signed-overflow UB; the two's-complement result is exact. */
void simd_scalar_i32_add(int32_t *restrict out, const int32_t *restrict a,
                         const int32_t *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

/* out[i] = a[i] * b[i], low 32 bits, like JS Math.imul. */
void simd_scalar_i32_mul(int32_t *restrict out, const int32_t *restrict a,
                         const int32_t *restrict b, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

/* out[i] = x[i] * s, low 32 bits, like Math.imul(x[i], s). */
void simd_scalar_i32_scale(int32_t *restrict out, const int32_t *restrict x,
                           int32_t s, size_t n) {
  for (size_t i = 0; i < n; i++)
    out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}

/* ── Inclusive prefix scans (out may alias x for in-place) ───────── */

/* out[i] = x[0]+..+x[i]. Sequential left-fold: this exact rounding order is the
 * oracle; the SIMD f32_cumsum reorders and matches only within tolerance. */
void simd_scalar_f32_cumsum(float *out, const float *x, size_t n) {
  float acc = 0.0f;
  for (size_t i = 0; i < n; i++) {
    acc += x[i];
    out[i] = acc;
  }
}

/* out[i] = x[0]+..+x[i], each step wrapping mod 2^32; exact vs any SIMD order. */
void simd_scalar_i32_cumsum(int32_t *out, const int32_t *x, size_t n) {
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    acc += (uint32_t)x[i];
    out[i] = (int32_t)acc;
  }
}

/* out[i] = max(x[0..i]). Identity -inf; uses `>` (NaN carries the running max
 * forward, never becoming the max itself). Exact — max rounds nothing. */
void simd_scalar_f32_cummax(float *out, const float *x, size_t n) {
  float acc = -INFINITY;
  for (size_t i = 0; i < n; i++) {
    if (x[i] > acc)
      acc = x[i];
    out[i] = acc;
  }
}

void simd_scalar_i32_cummax(int32_t *out, const int32_t *x, size_t n) {
  int32_t acc = INT32_MIN;
  for (size_t i = 0; i < n; i++) {
    if (x[i] > acc)
      acc = x[i];
    out[i] = acc;
  }
}

/* ── Override table ──────────────────────────────────────────────── */

/* ── forward value search (scalar fallback) ─────────────────────── */
static size_t simd_scalar_find_u8(const uint8_t *restrict p, uint8_t v,
                                  size_t n) {
  const void *r = memchr(p, v, n); /* libc memchr is already SIMD */
  return r ? (size_t)((const uint8_t *)r - p) : SIZE_MAX;
}
static size_t simd_scalar_find_u16(const uint16_t *restrict p, uint16_t v,
                                   size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_scalar_find_u32(const uint32_t *restrict p, uint32_t v,
                                   size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_scalar_find_f32(const float *restrict p, float v,
                                   size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_scalar_find_f64(const double *restrict p, double v,
                                   size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] == v) return i;
  return SIZE_MAX;
}

/* Scalar substring search: memchr to the first byte, then verify the rest.
 * Also the differential oracle the SIMD variants must match byte-for-byte. */
static size_t simd_scalar_strfind(const uint8_t *text, size_t n,
                                  const uint8_t *pat, size_t m) {
  if (m == 0) return 0;
  if (m > n) return SIZE_MAX;
  if (m == 1) {
    const void *r = memchr(text, pat[0], n);
    return r ? (size_t)((const uint8_t *)r - text) : SIZE_MAX;
  }
  size_t limit = n - m; /* last index at which a match can start */
  size_t i = 0;
  while (i <= limit) {
    const uint8_t *hit = memchr(text + i, pat[0], limit - i + 1);
    if (!hit) break;
    size_t pos = (size_t)(hit - text);
    if (memcmp(text + pos + 1, pat + 1, m - 1) == 0) return pos;
    i = pos + 1;
  }
  return SIZE_MAX;
}

/* ── Byte-scanning / text kernels (scalar reference + differential oracle) ── */

static size_t simd_scalar_count_u8(const uint8_t *restrict p, uint8_t v,
                                   size_t n) {
  size_t c = 0;
  for (size_t i = 0; i < n; i++)
    if (p[i] == v) c++;
  return c;
}

static size_t simd_scalar_find_first_of(const uint8_t *restrict p, size_t n,
                                        const uint8_t *restrict set,
                                        size_t setlen) {
  uint8_t tbl[256];
  if (setlen == 0) return SIZE_MAX;
  memset(tbl, 0, sizeof(tbl));
  for (size_t i = 0; i < setlen; i++) tbl[set[i]] = 1;
  for (size_t i = 0; i < n; i++)
    if (tbl[p[i]]) return i;
  return SIZE_MAX;
}

/* Strict UTF-8 validator: rejects overlong forms, surrogates (U+D800..DFFF),
 * and code points > U+10FFFF. Returns n if valid, else first invalid index. */
static size_t simd_scalar_validate_utf8(const uint8_t *restrict p, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = p[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) { i++; continue; }
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return i; /* stray continuation byte or 0xF8..0xFF */
    if (i + len > n) return i; /* truncated multibyte sequence */
    for (size_t j = 1; j < len; j++) {
      uint8_t cc = p[i + j];
      if ((cc & 0xC0) != 0x80) return i; /* not a continuation byte */
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 2 && cp < 0x80) return i;      /* overlong */
    if (len == 3 && cp < 0x800) return i;     /* overlong */
    if (len == 4 && cp < 0x10000) return i;   /* overlong */
    if (cp > 0x10FFFF) return i;              /* out of range */
    if (cp >= 0xD800 && cp <= 0xDFFF) return i; /* UTF-16 surrogate */
    i += len;
  }
  return n;
}

static const char simd_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t simd_scalar_base64_encode(const uint8_t *restrict src, size_t n,
                                        char *restrict dst) {
  const char *E = simd_b64_alphabet;
  size_t i = 0, o = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t x = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) |
                 src[i + 2];
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

/* Decode table, COMPILE-TIME. It used to be memset and refilled on every
 * call -- 256 bytes cleared plus 64 stores before decoding a single
 * character, which for a short input is the entire cost. A static const
 * table also has no initialiser to race on. */
static const int8_t simd_b64_dec[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static size_t simd_scalar_base64_decode(const char *restrict src, size_t n,
                                        uint8_t *restrict dst) {
  const int8_t *dec = simd_b64_dec;
  size_t i, o = 0;
  if (n % 4 != 0) return SIZE_MAX;
  if (n == 0) return 0;
  for (i = 0; i < n; i += 4) {
    int last = (i + 4 == n);
    uint8_t c0 = (uint8_t)src[i], c1 = (uint8_t)src[i + 1];
    uint8_t c2 = (uint8_t)src[i + 2], c3 = (uint8_t)src[i + 3];
    int v0 = dec[c0], v1 = dec[c1], v2, v3, pad = 0;
    uint32_t x;
    if (v0 < 0 || v1 < 0) return SIZE_MAX;
    if (c2 == '=') {
      if (!last || c3 != '=') return SIZE_MAX; /* '=' only at the tail */
      pad = 2; v2 = 0; v3 = 0;
    } else {
      v2 = dec[c2];
      if (v2 < 0) return SIZE_MAX;
      if (c3 == '=') {
        if (!last) return SIZE_MAX;
        pad = 1; v3 = 0;
      } else {
        v3 = dec[c3];
        if (v3 < 0) return SIZE_MAX;
      }
    }
    x = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) |
        (uint32_t)v3;
    dst[o++] = (x >> 16) & 0xFF;
    if (pad < 2) dst[o++] = (x >> 8) & 0xFF;
    if (pad < 1) dst[o++] = x & 0xFF;
  }
  return o;
}

/* ── hex / latin1 / utf8 byte kernels (scalar reference + differential oracle) ── */

static const char simd_hex_lut[] = "0123456789abcdef";

/* Hex-nibble value of a char: 0-9, a-f, A-F accepted; -1 for anything else. */
static inline int simd_hex_val(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void simd_scalar_hex_encode(const uint8_t *restrict src, size_t n,
                                   char *restrict dst) {
  size_t i, o = 0;
  for (i = 0; i < n; i++) {
    dst[o++] = simd_hex_lut[src[i] >> 4];
    dst[o++] = simd_hex_lut[src[i] & 0x0F];
  }
}

static size_t simd_scalar_hex_decode(const char *restrict src, size_t n,
                                     uint8_t *restrict dst) {
  size_t i, o = 0;
  if (n & 1) return SIZE_MAX;
  for (i = 0; i < n; i += 2) {
    int hi = simd_hex_val((uint8_t)src[i]);
    int lo = simd_hex_val((uint8_t)src[i + 1]);
    if (hi < 0 || lo < 0) return SIZE_MAX;
    dst[o++] = (uint8_t)((hi << 4) | lo);
  }
  return o;
}

static size_t simd_scalar_latin1_to_utf8(const uint8_t *restrict src, size_t n,
                                         uint8_t *restrict dst) {
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

static int simd_scalar_utf8_to_latin1(const uint8_t *restrict src, size_t n,
                                      uint8_t *restrict dst, size_t *out_len) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t c = src[i];
    if (c < 0x80) { dst[o++] = c; i++; continue; }
    if ((c & 0xE0) == 0xC0) { /* 2-byte lead 0xC0..0xDF */
      uint8_t c1;
      uint32_t cp;
      if (i + 1 >= n) return -1;              /* truncated */
      c1 = src[i + 1];
      if ((c1 & 0xC0) != 0x80) return -1;      /* bad continuation */
      cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
      if (cp < 0x80 || cp > 0xFF) return -1;   /* overlong, or won't fit latin1 */
      dst[o++] = (uint8_t)cp;
      i += 2;
      continue;
    }
    return -1; /* stray continuation, or 3-/4-byte lead => code point > 0xFF */
  }
  *out_len = o;
  return 0;
}

static size_t simd_scalar_count_utf8(const uint8_t *restrict p, size_t n) {
  size_t i, c = 0;
  for (i = 0; i < n; i++)
    if ((p[i] & 0xC0) != 0x80) c++; /* not a UTF-8 continuation byte */
  return c;
}

/* ── UTF-8 <-> UTF-16 transcode + validation (scalar reference + oracle) ──
 * These are the differential oracle the SIMD variants must match byte-for-byte.
 * UTF-16 units are host-endian uint16_t (== UTF-16LE on this engine's LE
 * targets). Both transcoders reject ill-formed input (lossless, no U+FFFD). */

static int simd_scalar_utf8_to_utf16le(const uint8_t *restrict src, size_t n,
                                       uint16_t *restrict dst,
                                       size_t *out_units) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t c = src[i];
    size_t len, j;
    uint32_t cp;
    if (c < 0x80) { dst[o++] = c; i++; continue; }
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return -1; /* stray continuation byte or 0xF8..0xFF */
    if (i + len > n) return -1; /* truncated multibyte sequence */
    for (j = 1; j < len; j++) {
      uint8_t cc = src[i + j];
      if ((cc & 0xC0) != 0x80) return -1; /* not a continuation byte */
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 2 && cp < 0x80) return -1;    /* overlong */
    if (len == 3 && cp < 0x800) return -1;   /* overlong */
    if (len == 4 && cp < 0x10000) return -1; /* overlong */
    if (cp > 0x10FFFF) return -1;            /* out of range */
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1; /* UTF-16 surrogate */
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

static int simd_scalar_utf16le_to_utf8(const uint16_t *restrict src,
                                       size_t units, uint8_t *restrict dst,
                                       size_t *out_len) {
  size_t i = 0, o = 0;
  while (i < units) {
    uint32_t cp = src[i];
    if (cp < 0xD800 || cp > 0xDFFF) {
      i++; /* BMP scalar value */
    } else if (cp <= 0xDBFF) {           /* high surrogate: need a low next */
      uint32_t lo;
      if (i + 1 >= units) return -1;     /* high surrogate at end of buffer */
      lo = src[i + 1];
      if (lo < 0xDC00 || lo > 0xDFFF) return -1; /* not a low surrogate */
      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      i += 2;
    } else {
      return -1; /* lone/leading low surrogate */
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

static int simd_scalar_validate_utf16le(const uint16_t *restrict src,
                                        size_t units) {
  size_t i = 0;
  while (i < units) {
    uint32_t c = src[i];
    if (c < 0xD800 || c > 0xDFFF) { i++; continue; } /* BMP */
    if (c > 0xDBFF) return 0;                         /* lone low surrogate */
    if (i + 1 >= units) return 0;                     /* high at end */
    if (src[i + 1] < 0xDC00 || src[i + 1] > 0xDFFF) return 0; /* not low */
    i += 2;
  }
  return 1;
}

static size_t simd_scalar_count_utf16(const uint16_t *restrict src,
                                      size_t units) {
  size_t i, c = 0;
  for (i = 0; i < units; i++)
    if (src[i] < 0xDC00 || src[i] > 0xDFFF) c++; /* not a low surrogate */
  return c;
}

/* Portable membership scan: one shift, one index, one bit test per byte. */
static size_t simd_scalar_find_bitmap(const uint8_t *restrict p, size_t n,
                                      const uint8_t *restrict bitmap) {
  size_t i;
  for (i = 0; i < n; i++)
    if (bitmap[p[i] >> 3] & (uint8_t)(1u << (p[i] & 7))) return i;
  return SIZE_MAX;
}

void simd_override_scalar(simd_t *t) {
  t->find_u8 = simd_scalar_find_u8;
  t->find_u16 = simd_scalar_find_u16;
  t->find_u32 = simd_scalar_find_u32;
  t->find_f32 = simd_scalar_find_f32;
  t->find_f64 = simd_scalar_find_f64;
  t->strfind = simd_scalar_strfind;
  t->count_u8 = simd_scalar_count_u8;
  t->find_first_of = simd_scalar_find_first_of;
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