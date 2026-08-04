#include "dyna-simd-kernels.h"
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

/* AVX2 + FMA SIMD overrides — 256-bit YMM registers, 8 floats per vector.
 * Compiled only on x86-64 with -mavx2,-mfma via target attribute.
 * This is the highest-performance portable x86 SIMD path below AVX-512. */

/* Whole-file guard: pairs with the #else/#endif stub at the bottom. A
 * stray early #endif here used to orphan that #else, so this file never
 * compiled on any host. */
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <math.h>
#include <string.h>
#include "dyna-simd-kernels.h"

/* Scalar fallbacks (simd_scalar.c) installed for ops that have
 * no AVX2 kernel; not declared in the shared header. */
void simd_scalar_axpy(float *restrict y, float alpha,
                             const float *restrict x, size_t n);
void simd_scalar_axpby(float *restrict z, float a,
                              const float *restrict x, float b,
                              const float *restrict y, size_t n);
#include <math.h>

/* ── Horizontal sum helpers ──────────────────────────────────── */
__attribute__((target("avx2,fma"))) static inline float
hsum256_ps(__m256 v) {
  __m128 lo = _mm256_castps256_ps128(v);
  __m128 hi = _mm256_extractf128_ps(v, 1);
  __m128 s = _mm_add_ps(lo, hi);
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
  return _mm_cvtss_f32(s);
}

__attribute__((target("avx2,fma"), unused)) static inline void
hsum256_ps_pair(__m256 v, float *out) {
  __m128 lo = _mm256_castps256_ps128(v);
  __m128 hi = _mm256_extractf128_ps(v, 1);
  _mm_storeu_ps(out, _mm_add_ps(lo, hi));
}

/* ── Dot product (float accumulator) ─────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_dot(const float *restrict a, const float *restrict b,
                     size_t n) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    acc = _mm256_fmadd_ps(va, vb, acc);
  }
  float result = hsum256_ps(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

/* ── Dot product (double accumulator for numerical stability) ── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_dot_f(const float *restrict a, const float *restrict b,
                       size_t n) {
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    __m256 prod = _mm256_mul_ps(va, vb);
    acc0 = _mm256_add_pd(acc0, _mm256_cvtps_pd(_mm256_castps256_ps128(prod)));
    acc1 = _mm256_add_pd(acc1, _mm256_cvtps_pd(_mm256_extractf128_ps(prod, 1)));
  }
  double result = acc0[0] + acc0[1] + acc1[0] + acc1[1];
  for (; i < n; i++)
    result += (double)a[i] * b[i];
  return (float)result;
}

/* ── Norm L2 squared ────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_norm_l2_sq(const float *restrict x, size_t n) {
  return simd_avx2_dot(x, x, n);
}

__attribute__((target("avx2,fma"))) static float
simd_avx2_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_avx2_norm_l2_sq(x, n));
}

/* ── Norm L1 ─────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_norm_l1(const float *restrict x, size_t n) {
  __m256 acc = _mm256_setzero_ps();
  __m256 sign_mask = _mm256_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vx = _mm256_loadu_ps(&x[i]);
    acc = _mm256_add_ps(acc, _mm256_andnot_ps(sign_mask, vx));
  }
  float result = hsum256_ps(acc);
  for (; i < n; i++)
    result += fabsf(x[i]);
  return result;
}

/* ── Sum ─────────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_sum(const float *restrict x, size_t n) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    acc = _mm256_add_ps(acc, _mm256_loadu_ps(&x[i]));
  float result = hsum256_ps(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

/* ── Max ─────────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_max(const float *restrict x, size_t n) {
  if (n == 0)
    return -FLT_MAX;
  /* The 8-wide head load is only safe when n >= 8. The old reduction ran
   * hsum256_ps (a horizontal SUM) over the lane maxes — not a max. Reduce
   * with a real horizontal max (hi/lo 128 max, then movehl + max_ss). */
  float result;
  size_t i;
  if (n >= 8) {
    __m256 vmax = _mm256_loadu_ps(x);
    for (i = 8; i + 8 <= n; i += 8)
      vmax = _mm256_max_ps(vmax, _mm256_loadu_ps(&x[i]));
    __m128 m = _mm_max_ps(_mm256_castps256_ps128(vmax),
                          _mm256_extractf128_ps(vmax, 1));
    m = _mm_max_ps(m, _mm_movehl_ps(m, m));
    m = _mm_max_ss(m, _mm_shuffle_ps(m, m, 1));
    result = _mm_cvtss_f32(m);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

/* ── Min ─────────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_min(const float *restrict x, size_t n) {
  if (n == 0)
    return FLT_MAX;
  /* See simd_avx2_max: the old hsum256_ps reduction was a SUM, not a min.
   * Real horizontal min; guard the head load for n < 8. */
  float result;
  size_t i;
  if (n >= 8) {
    __m256 vmin = _mm256_loadu_ps(x);
    for (i = 8; i + 8 <= n; i += 8)
      vmin = _mm256_min_ps(vmin, _mm256_loadu_ps(&x[i]));
    __m128 m = _mm_min_ps(_mm256_castps256_ps128(vmin),
                          _mm256_extractf128_ps(vmin, 1));
    m = _mm_min_ps(m, _mm_movehl_ps(m, m));
    m = _mm_min_ss(m, _mm_shuffle_ps(m, m, 1));
    result = _mm_cvtss_f32(m);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

/* ── Argmax ──────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static size_t
simd_avx2_argmax(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m256 vmax = _mm256_loadu_ps(x);
  __m256i vidx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  __m256i vidx_max = vidx;
  size_t i = 8;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&x[i]);
    __m256i vidxi = _mm256_set1_epi32((int)i);
    __m256i step = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    vidxi = _mm256_add_epi32(vidxi, step);
    __m256 mask = _mm256_cmp_ps(vi, vmax, _CMP_GT_OS);
    vmax = _mm256_max_ps(vmax, vi);
    vidx_max = _mm256_castps_si256(_mm256_blendv_ps(
        _mm256_castsi256_ps(vidx_max), _mm256_castsi256_ps(vidxi), mask));
  }
  float tmp[8];
  uint32_t idx_tmp[8];
  _mm256_storeu_ps(tmp, vmax);
  _mm256_storeu_si256((__m256i *)idx_tmp, vidx_max);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 8; k++)
    if (tmp[k] > best) {
      best = tmp[k];
      best_idx = idx_tmp[k];
    }
  for (; i < n; i++)
    if (x[i] > best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argmin ──────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static size_t
simd_avx2_argmin(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m256 vmin = _mm256_loadu_ps(x);
  __m256i vidx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  __m256i vidx_min = vidx;
  size_t i = 8;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&x[i]);
    __m256i vidxi = _mm256_set1_epi32((int)i);
    __m256i step = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    vidxi = _mm256_add_epi32(vidxi, step);
    __m256 mask = _mm256_cmp_ps(vi, vmin, _CMP_LT_OS);
    vmin = _mm256_min_ps(vmin, vi);
    vidx_min = _mm256_castps_si256(_mm256_blendv_ps(
        _mm256_castsi256_ps(vidx_min), _mm256_castsi256_ps(vidxi), mask));
  }
  float tmp[8];
  uint32_t idx_tmp[8];
  _mm256_storeu_ps(tmp, vmin);
  _mm256_storeu_si256((__m256i *)idx_tmp, vidx_min);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 8; k++)
    if (tmp[k] < best) {
      best = tmp[k];
      best_idx = idx_tmp[k];
    }
  for (; i < n; i++)
    if (x[i] < best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argminmax ───────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_argminmax(const float *restrict x, size_t n,
                           size_t *argmin_out, size_t *argmax_out) {
  if (n == 0) {
    *argmin_out = *argmax_out = 0;
    return;
  }
  __m256 vmin = _mm256_loadu_ps(x);
  __m256 vmax = vmin;
  __m256i vidx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
  __m256i vidx_min = vidx, vidx_max = vidx;
  size_t i = 8;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&x[i]);
    __m256i vidxi = _mm256_set1_epi32((int)i);
    __m256i step = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    vidxi = _mm256_add_epi32(vidxi, step);
    __m256 mask_lt = _mm256_cmp_ps(vi, vmin, _CMP_LT_OS);
    __m256 mask_gt = _mm256_cmp_ps(vi, vmax, _CMP_GT_OS);
    vmin = _mm256_min_ps(vmin, vi);
    vmax = _mm256_max_ps(vmax, vi);
    vidx_min = _mm256_castps_si256(_mm256_blendv_ps(
        _mm256_castsi256_ps(vidx_min), _mm256_castsi256_ps(vidxi), mask_lt));
    vidx_max = _mm256_castps_si256(_mm256_blendv_ps(
        _mm256_castsi256_ps(vidx_max), _mm256_castsi256_ps(vidxi), mask_gt));
  }
  float tmp_min[8], tmp_max[8];
  uint32_t idx_min[8], idx_max[8];
  _mm256_storeu_ps(tmp_min, vmin);
  _mm256_storeu_ps(tmp_max, vmax);
  _mm256_storeu_si256((__m256i *)idx_min, vidx_min);
  _mm256_storeu_si256((__m256i *)idx_max, vidx_max);
  float best_min = tmp_min[0], best_max = tmp_max[0];
  size_t imin = idx_min[0], imax = idx_max[0];
  for (size_t k = 1; k < 8; k++) {
    if (tmp_min[k] < best_min) {
      best_min = tmp_min[k];
      imin = idx_min[k];
    }
    if (tmp_max[k] > best_max) {
      best_max = tmp_max[k];
      imax = idx_max[k];
    }
  }
  for (; i < n; i++) {
    if (x[i] < best_min) {
      best_min = x[i];
      imin = i;
    }
    if (x[i] > best_max) {
      best_max = x[i];
      imax = i;
    }
  }
  *argmin_out = imin;
  *argmax_out = imax;
}

/* ── Element-wise: vector–vector ─────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_add(float *restrict z, const float *restrict a,
                     const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    _mm256_storeu_ps(&z[i], _mm256_add_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] + b[i];
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_sub(float *restrict z, const float *restrict a,
                     const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    _mm256_storeu_ps(&z[i], _mm256_sub_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] - b[i];
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_mul(float *restrict z, const float *restrict a,
                     const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    _mm256_storeu_ps(&z[i], _mm256_mul_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] * b[i];
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_div(float *restrict z, const float *restrict a,
                     const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    _mm256_storeu_ps(&z[i], _mm256_div_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] / b[i];
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_abs(float *restrict out, const float *restrict in,
                     size_t n) {
  __m256 sign_mask = _mm256_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    _mm256_storeu_ps(&out[i], _mm256_andnot_ps(sign_mask, vi));
  }
  for (; i < n; i++)
    out[i] = fabsf(in[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_fma_krn(float *restrict z, const float *restrict a,
                         const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vz = _mm256_loadu_ps(&z[i]);
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    _mm256_storeu_ps(&z[i], _mm256_fmadd_ps(va, vb, vz));
  }
  for (; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ──────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_add_s(float *restrict z, const float *restrict x,
                       float s, size_t n) {
  __m256 vs = _mm256_set1_ps(s);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vx = _mm256_loadu_ps(&x[i]);
    _mm256_storeu_ps(&z[i], _mm256_add_ps(vx, vs));
  }
  for (; i < n; i++)
    z[i] = x[i] + s;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_mul_s(float *restrict z, const float *restrict x,
                       float s, size_t n) {
  __m128 vs = _mm_set1_ps(s);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vx = _mm256_loadu_ps(&x[i]);
    _mm256_storeu_ps(&z[i], _mm256_mul_ps(vx, _mm256_broadcast_ps(&vs)));
  }
  for (; i < n; i++)
    z[i] = x[i] * s;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_scale_add_s(float *restrict z, float alpha,
                             const float *restrict x, float beta,
                             size_t n) {
  __m256 va = _mm256_set1_ps(alpha);
  __m256 vb = _mm256_set1_ps(beta);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vx = _mm256_loadu_ps(&x[i]);
    _mm256_storeu_ps(&z[i], _mm256_fmadd_ps(va, vx, vb));
  }
  for (; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ──────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_sigmoid(float *restrict out, const float *restrict in,
                         size_t n) {
  __m256 one = _mm256_set1_ps(1.0f);
  __m256 lo = _mm256_set1_ps(-30.0f);
  __m256 hi = _mm256_set1_ps(30.0f);
  __m256 magic = _mm256_set1_ps(-12102203.0f);
  __m256 bias = _mm256_set1_ps(1.0f);
  __m256i zero = _mm256_setzero_si256();
  __m256i top = _mm256_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    vi = _mm256_max_ps(_mm256_min_ps(vi, hi), lo);
    __m256i bits = _mm256_cvtps_epi32(_mm256_mul_ps(vi, magic));
    bits = _mm256_add_epi32(bits, _mm256_castps_si256(bias));
    bits = _mm256_max_epi32(bits, zero);
    bits = _mm256_min_epi32(bits, top);
    __m256 ve = _mm256_add_ps(one, _mm256_castsi256_ps(bits));
    _mm256_storeu_ps(&out[i], _mm256_div_ps(one, ve));
  }
  for (; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_tanh_fast(float *restrict out,
                           const float *restrict in, size_t n) {
  __m256 two = _mm256_set1_ps(2.0f);
  __m256 one = _mm256_set1_ps(1.0f);
  __m256 lo = _mm256_set1_ps(-10.0f);
  __m256 hi = _mm256_set1_ps(10.0f);
  __m256 magic = _mm256_set1_ps(-12102203.0f * 2.0f);
  __m256 bias = _mm256_set1_ps(1.0f);
  __m256i zero = _mm256_setzero_si256();
  __m256i top = _mm256_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    vi = _mm256_max_ps(_mm256_min_ps(vi, hi), lo);
    __m256i bits = _mm256_cvtps_epi32(_mm256_mul_ps(vi, magic));
    bits = _mm256_add_epi32(bits, _mm256_castps_si256(bias));
    bits = _mm256_max_epi32(bits, zero);
    bits = _mm256_min_epi32(bits, top);
    __m256 ve = _mm256_sub_ps(
        _mm256_div_ps(two, _mm256_add_ps(one, _mm256_castsi256_ps(bits))), one);
    _mm256_storeu_ps(&out[i], ve);
  }
  for (; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_gelu(float *restrict out, const float *restrict in,
                      size_t n) {
  const __m256 sqrt_2_over_pi = _mm256_set1_ps(0.7978845608028654f);
  const __m256 c = _mm256_set1_ps(0.044715f);
  const __m256 half = _mm256_set1_ps(0.5f);
  const __m256 one = _mm256_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 x = _mm256_loadu_ps(&in[i]);
    __m256 x3 = _mm256_mul_ps(_mm256_mul_ps(x, x), x);
    __m256 inner = _mm256_mul_ps(sqrt_2_over_pi, _mm256_fmadd_ps(c, x3, x));
    /* _mm256_tanh_ps is SVML-only (ICC); per-lane libm tanh keeps the
     * lanes bit-identical to the scalar tail on GCC/Clang. */
    {
      float tmp[8];
      _mm256_storeu_ps(tmp, inner);
      for (int j = 0; j < 8; j++)
        tmp[j] = tanhf(tmp[j]);
      inner = _mm256_loadu_ps(tmp);
    }
    _mm256_storeu_ps(&out[i], _mm256_mul_ps(_mm256_mul_ps(half, x),
                                            _mm256_add_ps(one, inner)));
  }
  for (; i < n; i++) {
    float x = in[i];
    float x3 = x * x * x;
    float inner = tanhf(0.7978845608028654f * (x + 0.044715f * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_silu(float *restrict out, const float *restrict in,
                      size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 x = _mm256_loadu_ps(&in[i]);
    __m256 neg_x = _mm256_sub_ps(_mm256_setzero_ps(), x);
    __m256 exp_neg_x;
    /* Use fast exp approximation inline */
    /* The magic is POSITIVE: a negative one double-negates against neg_x and
       silently computes silu(-x) (see sse42/neon, which had this bug). */
    __m256i bits =
        _mm256_cvtps_epi32(_mm256_mul_ps(neg_x, _mm256_set1_ps(12102203.0f)));
    bits = _mm256_add_epi32(bits, _mm256_castps_si256(_mm256_set1_ps(1.0f)));
    bits = _mm256_max_epi32(bits, _mm256_setzero_si256());
    bits = _mm256_min_epi32(bits, _mm256_set1_epi32(0x7F800000));
    exp_neg_x = _mm256_max_ps(_mm256_castsi256_ps(bits), _mm256_setzero_ps());
    __m256 sigmoid_x = _mm256_div_ps(
        _mm256_set1_ps(1.0f), _mm256_add_ps(_mm256_set1_ps(1.0f), exp_neg_x));
    _mm256_storeu_ps(&out[i], _mm256_mul_ps(x, sigmoid_x));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v / (1.0f + expf(-v));
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_relu(float *restrict out, const float *restrict in,
                      size_t n) {
  __m256 zero = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    _mm256_storeu_ps(&out[i], _mm256_max_ps(vi, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_relu6(float *restrict out, const float *restrict in,
                       size_t n) {
  __m256 zero = _mm256_setzero_ps();
  __m256 six = _mm256_set1_ps(6.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    _mm256_storeu_ps(&out[i], _mm256_min_ps(_mm256_max_ps(vi, zero), six));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_leaky_relu(float *restrict out,
                            const float *restrict in, float slope,
                            size_t n) {
  __m256 vzero = _mm256_setzero_ps();
  __m256 vslope = _mm256_set1_ps(slope);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vzero, _CMP_GT_OS);
    __m256 res = _mm256_blendv_ps(_mm256_mul_ps(vi, vslope), vi, mask);
    _mm256_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_elu(float *restrict out, const float *restrict in,
                     float alpha, size_t n) {
  __m256 vzero = _mm256_setzero_ps();
  __m256 valpha = _mm256_set1_ps(alpha);
  __m256 vone = _mm256_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vzero, _CMP_GT_OS);
    /* _mm256_exp_ps is SVML-only; per-lane libm exp matches the tail. */
    __m256 vexp;
    {
      float tmp[8];
      _mm256_storeu_ps(tmp, vi);
      for (int j = 0; j < 8; j++)
        tmp[j] = expf(tmp[j]);
      vexp = _mm256_loadu_ps(tmp);
    }
    __m256 exp_part = _mm256_sub_ps(vexp, vone);
    __m256 res = _mm256_blendv_ps(_mm256_mul_ps(valpha, exp_part), vi, mask);
    _mm256_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

/* Horizontal max of 8 lanes (the old code summed lanes via
 * hsum256_ps, which is not a max reduction at all). */
__attribute__((target("avx2,fma"))) static inline float
avx2_hmax256_ps(__m256 v) {
  __m128 m = _mm_max_ps(_mm256_castps256_ps128(v),
                        _mm256_extractf128_ps(v, 1));
  m = _mm_max_ps(m, _mm_movehl_ps(m, m));
  m = _mm_max_ss(m, _mm_shuffle_ps(m, m, 1));
  return _mm_cvtss_f32(m);
}

/* Vectorized Schraudolph exp, matching fast_exp bit-for-bit:
 * bits = (int)(12102203·x + 0x3F800000), clamped to [0, +inf bits]. */
__attribute__((target("avx2,fma"))) static inline __m256
avx2_fast_exp_ps(__m256 x) {
  __m256i bits = _mm256_cvtps_epi32(_mm256_fmadd_ps(
      x, _mm256_set1_ps(12102203.0f), _mm256_set1_ps(1065353216.0f)));
  bits = _mm256_max_epi32(bits, _mm256_setzero_si256());
  bits = _mm256_min_epi32(bits, _mm256_set1_epi32(0x7F800000));
  return _mm256_castsi256_ps(bits);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_softmax(float *restrict out, const float *restrict in,
                         size_t n) {
  if (unlikely(n == 0))
    return;
  /* Max reduction. The 8-wide head load is only safe when n >= 8 —
   * softmax is routinely called with tiny n (class counts). */
  float maxv = in[0];
  size_t i = 1;
  if (n >= 8) {
    __m256 vmax = _mm256_loadu_ps(in);
    for (i = 8; i + 8 <= n; i += 8)
      vmax = _mm256_max_ps(vmax, _mm256_loadu_ps(&in[i]));
    maxv = avx2_hmax256_ps(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  __m256 vmaxv = _mm256_set1_ps(maxv);
  i = 0;
  for (; i + 8 <= n; i += 8)
    _mm256_storeu_ps(
        &out[i], avx2_fast_exp_ps(_mm256_sub_ps(_mm256_loadu_ps(&in[i]), vmaxv)));
  for (; i < n; i++)
    out[i] = fast_exp(in[i] - maxv);

  /* Compute the sum once, before any element is scaled. */
  float inv_sum = 1.0f / simd_hsum_f32(out, n);
  __m256 vinv = _mm256_set1_ps(inv_sum);
  for (i = 0; i + 8 <= n; i += 8)
    _mm256_storeu_ps(&out[i], _mm256_mul_ps(_mm256_loadu_ps(&out[i]), vinv));
  for (; i < n; i++)
    out[i] *= inv_sum;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_log_softmax(float *restrict out,
                             const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  float maxv = in[0];
  size_t i = 1;
  if (n >= 8) {
    __m256 vmax = _mm256_loadu_ps(in);
    for (i = 8; i + 8 <= n; i += 8)
      vmax = _mm256_max_ps(vmax, _mm256_loadu_ps(&in[i]));
    maxv = avx2_hmax256_ps(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* Accumulate the exp-sum in a register. Storing exps into out[] would
   * clobber in[] when out aliases in (the binding calls log_softmax(a, a, n)),
   * and the final in[i]-voff loop still needs the original input — the old
   * "exps land in out[]" code produced a ~550% error on in-place input. */
  __m256 vmaxv = _mm256_set1_ps(maxv);
  __m256 vsum = _mm256_setzero_ps();
  i = 0;
  for (; i + 8 <= n; i += 8)
    vsum = _mm256_add_ps(
        vsum, avx2_fast_exp_ps(_mm256_sub_ps(_mm256_loadu_ps(&in[i]), vmaxv)));
  float sum = hsum256_ps(vsum);
  for (; i < n; i++)
    sum += fast_exp(in[i] - maxv);

  float log_s = logf(sum);
  __m256 voff = _mm256_set1_ps(maxv + log_s);
  for (i = 0; i + 8 <= n; i += 8)
    _mm256_storeu_ps(&out[i], _mm256_sub_ps(_mm256_loadu_ps(&in[i]), voff));
  for (; i < n; i++)
    out[i] = in[i] - maxv - log_s;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_vexp(float *restrict out, const float *restrict in,
                      size_t n) {
  __m256 magic = _mm256_set1_ps(12102203.0f);
  __m256 bias = _mm256_set1_ps(1.0f);
  __m256 vzero = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256i bits = _mm256_cvtps_epi32(_mm256_mul_ps(vi, magic));
    bits = _mm256_add_epi32(bits, _mm256_castps_si256(bias));
    bits = _mm256_max_epi32(bits, _mm256_setzero_si256());
    bits = _mm256_min_epi32(bits, _mm256_set1_epi32(0x7F800000));
    __m256 ve = _mm256_castsi256_ps(bits);
    _mm256_storeu_ps(&out[i], _mm256_max_ps(ve, vzero));
  }
  for (; i < n; i++)
    out[i] = fast_exp(in[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_vlog(float *restrict out, const float *restrict in,
                      size_t n) {
  __m256 vzero = _mm256_setzero_ps();
  __m256 vneg = _mm256_set1_ps(-FLT_MAX);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vzero, _CMP_GT_OS);
    /* _mm256_log_ps is SVML-only; per-lane libm log matches the tail. */
    __m256 vln;
    {
      float tmp[8];
      _mm256_storeu_ps(tmp, vi);
      for (int j = 0; j < 8; j++)
        tmp[j] = logf(tmp[j]);
      vln = _mm256_loadu_ps(tmp);
    }
    vln = _mm256_blendv_ps(vneg, vln, mask);
    _mm256_storeu_ps(&out[i], vln);
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_vsqrt(float *restrict out, const float *restrict in,
                       size_t n) {
  __m256 vzero = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vzero, _CMP_GE_OS);
    __m256 vs = _mm256_sqrt_ps(vi);
    vs = _mm256_blendv_ps(vzero, vs, mask);
    _mm256_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_vrsqrt(float *restrict out, const float *restrict in,
                        size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    _mm256_storeu_ps(&out[i], _mm256_rsqrt_ps(vi));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_vinv(float *restrict out, const float *restrict in,
                      size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    _mm256_storeu_ps(&out[i], _mm256_rcp_ps(vi));
  }
  for (; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_dist_l2_sq(const float *restrict a,
                            const float *restrict b, size_t d) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= d; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    __m256 diff = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(diff, diff, acc);
  }
  float result = hsum256_ps(acc);
  for (; i < d; i++) {
    float df = a[i] - b[i];
    result += df * df;
  }
  return result;
}

__attribute__((target("avx2,fma"))) static float
simd_avx2_dist_l1(const float *restrict a,
                         const float *restrict b, size_t d) {
  __m256 acc = _mm256_setzero_ps();
  __m256 sign_mask = _mm256_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 8 <= d; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    __m256 diff = _mm256_sub_ps(va, vb);
    acc = _mm256_add_ps(acc, _mm256_andnot_ps(sign_mask, diff));
  }
  float result = hsum256_ps(acc);
  for (; i < d; i++)
    result += fabsf(a[i] - b[i]);
  return result;
}

__attribute__((target("avx2,fma"))) static float
simd_avx2_dist_cos(const float *restrict a,
                          const float *restrict b, size_t d) {
  __m256 vdot = _mm256_setzero_ps();
  __m256 vna = _mm256_setzero_ps();
  __m256 vnb = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= d; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    vdot = _mm256_fmadd_ps(va, vb, vdot);
    vna = _mm256_fmadd_ps(va, va, vna);
    vnb = _mm256_fmadd_ps(vb, vb, vnb);
  }
  float dot = hsum256_ps(vdot);
  float na = hsum256_ps(vna);
  float nb = hsum256_ps(vnb);
  for (; i < d; i++) {
    dot += (double)a[i] * b[i];
    na += (double)a[i] * a[i];
    nb += (double)b[i] * b[i];
  }
  double denom = sqrt(na * nb);
  if (denom < FLT_MIN)
    return 1.0f;
  return (float)(1.0 - dot / denom);
}

__attribute__((target("avx2,fma"))) static float
simd_avx2_dist_cheb(const float *restrict a,
                           const float *restrict b, size_t d) {
  __m256 vmax = _mm256_setzero_ps();
  __m256 sign_mask = _mm256_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 8 <= d; i += 8) {
    __m256 va = _mm256_loadu_ps(&a[i]);
    __m256 vb = _mm256_loadu_ps(&b[i]);
    __m256 adiff = _mm256_andnot_ps(sign_mask, _mm256_sub_ps(va, vb));
    vmax = _mm256_max_ps(vmax, adiff);
  }
  /* Real horizontal max (Chebyshev = max abs diff); the old hsum256_ps
   * summed the lane maxes instead. */
  float result = avx2_hmax256_ps(vmax);
  for (; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > result)
      result = df;
  }
  return result;
}

/* ── Distance matrix ──────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_dist_matrix_l2_sq(float *restrict out,
                                   const float *restrict a,
                                   const float *restrict b, size_t n,
                                   size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx2_dist_l2_sq(&a[i * d], &b[j * d], d);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_dist_matrix_cos(float *restrict out,
                                 const float *restrict a,
                                 const float *restrict b, size_t n,
                                 size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx2_dist_cos(&a[i * d], &b[j * d], d);
}

__attribute__((target("avx2,fma"))) static void simd_avx2_dist_matrix_l1(
    float *restrict out, const float *restrict a,
    const float *restrict b, size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx2_dist_l1(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: gemv ───────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_gemv(float *restrict y, const float *restrict a,
                      const float *restrict x, size_t m, size_t n,
                      float beta) {
  for (size_t i = 0; i < m; i++) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    size_t j = 0;
    for (; j + 16 <= n; j += 16) {
      acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(&a[i * n + j]),
                             _mm256_loadu_ps(&x[j]), acc0);
      acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(&a[i * n + j + 8]),
                             _mm256_loadu_ps(&x[j + 8]), acc1);
    }
    for (; j + 8 <= n; j += 8)
      acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(&a[i * n + j]),
                             _mm256_loadu_ps(&x[j]), acc0);
    acc0 = _mm256_add_ps(acc0, acc1);
    float result = hsum256_ps(acc0);
    for (; j < n; j++)
      result += a[i * n + j] * x[j];
    y[i] = beta * y[i] + result;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_gemv_t(float *restrict y, const float *restrict a,
                        const float *restrict x, size_t m, size_t n,
                        float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    __m256 vxi = _mm256_set1_ps(x[i]);
    const float *row = &a[i * n];
    size_t j = 0;
    for (; j + 8 <= n; j += 8) {
      __m256 vy = _mm256_loadu_ps(&y[j]);
      __m256 va = _mm256_loadu_ps(&row[j]);
      _mm256_storeu_ps(&y[j], _mm256_fmadd_ps(vxi, va, vy));
    }
    for (; j < n; j++)
      y[j] += x[i] * row[j];
  }
}

/* ── BLAS Level 3: gemm (cache-tiled) ─────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_gemm(float *restrict c, const float *restrict a,
                      const float *restrict b, size_t m, size_t n, size_t k,
                      float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j + 8 <= n; j += 8) {
      __m256 vc = _mm256_loadu_ps(&c[i * n + j]);
      vc = _mm256_mul_ps(vc, _mm256_set1_ps(beta));
      _mm256_storeu_ps(&c[i * n + j], vc);
    }
  for (size_t j = n - (n % 8); j < n; j++)
    for (size_t i = 0; i < m; i++)
      c[i * n + j] *= beta;

  for (size_t i0 = 0; i0 < m; i0 += T) {
    size_t imax = i0 + T < m ? i0 + T : m;
    for (size_t j0 = 0; j0 < n; j0 += T) {
      size_t jmax = j0 + T < n ? j0 + T : n;
      for (size_t k0 = 0; k0 < k; k0 += T) {
        size_t kmax = k0 + T < k ? k0 + T : k;
        for (size_t i = i0; i < imax; i++) {
          for (size_t kk = k0; kk < kmax; kk++) {
            __m256 vaik = _mm256_set1_ps(alpha * a[i * k + kk]);
            size_t j = j0;
            for (; j + 8 <= jmax; j += 8) {
              __m256 vb = _mm256_loadu_ps(&b[kk * n + j]);
              __m256 vc = _mm256_loadu_ps(&c[i * n + j]);
              _mm256_storeu_ps(&c[i * n + j], _mm256_fmadd_ps(vaik, vb, vc));
            }
            for (; j < jmax; j++)
              c[i * n + j] += alpha * a[i * k + kk] * b[kk * n + j];
          }
        }
      }
    }
  }
}

/* ── Comparison / Selection ───────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_threshold(float *restrict out,
                           const float *restrict in, float t, size_t n) {
  __m256 vt = _mm256_set1_ps(t);
  __m256 one = _mm256_set1_ps(1.0f);
  __m256 zero = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vt, _CMP_GT_OS);
    _mm256_storeu_ps(&out[i], _mm256_blendv_ps(zero, one, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

__attribute__((target("avx2,fma"))) static void simd_avx2_threshold_sign(
    float *restrict out, const float *restrict in, float t, size_t n) {
  __m256 vt = _mm256_set1_ps(t);
  __m256 pos = _mm256_set1_ps(1.0f);
  __m256 neg = _mm256_set1_ps(-1.0f);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 mask = _mm256_cmp_ps(vi, vt, _CMP_GE_OS);
    _mm256_storeu_ps(&out[i], _mm256_blendv_ps(neg, pos, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ──────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static float
simd_avx2_hamming(const uint32_t *restrict a,
                         const uint32_t *restrict b, size_t n_words) {
  /* _mm256_popcnt_epi32 needs AVX512-VPOPCNTDQ, which this AVX2 path
   * cannot assume. Scalar POPCNT over 64-bit words is the portable
   * fast path here. */
  uint64_t result = 0;
  size_t i = 0;
  for (; i + 2 <= n_words; i += 2) {
    uint64_t wa, wb;
    memcpy(&wa, &a[i], sizeof(wa));
    memcpy(&wb, &b[i], sizeof(wb));
    result += (uint64_t)__builtin_popcountll(wa ^ wb);
  }
  for (; i < n_words; i++)
    result += (uint64_t)__builtin_popcount(a[i] ^ b[i]);
  return (float)result;
}

/* ── Top-K (scalar heap — ok for k << n) ──────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_topk_indices(const float *restrict vals,
                              uint32_t *restrict indices, size_t n,
                              size_t k) {
  simd_scalar_topk_indices(vals, indices, n, k);
}

/* ── Clamp ──────────────────────────────────────────────────────── */
__attribute__((target("avx2,fma"))) static void
simd_avx2_clamp(float *restrict out, const float *restrict in,
                       float lo, float hi, size_t n) {
  __m256 vlo = _mm256_set1_ps(lo);
  __m256 vhi = _mm256_set1_ps(hi);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 vi = _mm256_loadu_ps(&in[i]);
    __m256 clamped = _mm256_min_ps(_mm256_max_ps(vi, vlo), vhi);
    _mm256_storeu_ps(&out[i], clamped);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* Substring search, first+last algorithm over 32-byte blocks (AVX2). */
__attribute__((target("avx2,fma"))) static size_t
simd_avx2_strfind(const uint8_t *text, size_t n, const uint8_t *pat, size_t m) {
  if (m == 0) return 0;
  if (m > n) return SIZE_MAX;
  if (m == 1) {
    const void *r = memchr(text, pat[0], n);
    return r ? (size_t)((const uint8_t *)r - text) : SIZE_MAX;
  }
  __m256i vfirst = _mm256_set1_epi8((char)pat[0]);
  __m256i vlast = _mm256_set1_epi8((char)pat[m - 1]);
  size_t i = 0;
  while (i + 32 + (m - 1) <= n) {
    __m256i bf = _mm256_loadu_si256((const __m256i *)(text + i));
    __m256i bl = _mm256_loadu_si256((const __m256i *)(text + i + m - 1));
    unsigned mask = (unsigned)_mm256_movemask_epi8(_mm256_and_si256(
        _mm256_cmpeq_epi8(bf, vfirst), _mm256_cmpeq_epi8(bl, vlast)));
    while (mask) {
      int j = __builtin_ctz(mask);
      if (m == 2 || memcmp(text + i + j + 1, pat + 1, m - 2) == 0)
        return i + j;
      mask &= mask - 1;
    }
    i += 32;
  }
  size_t limit = n - m;
  while (i <= limit) {
    if (text[i] == pat[0] && memcmp(text + i + 1, pat + 1, m - 1) == 0)
      return i;
    i++;
  }
  return SIZE_MAX;
}

/* ── hex / latin1 / utf8 byte kernels (AVX2) ─────────────────────── */

static const char simd_avx2_hexc[] = "0123456789abcdef";

static inline int simd_avx2_hexval(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Encode 32 bytes -> 64 hex chars. PSHUFB is per-128-lane, so the 16-byte LUT
 * is broadcast into both lanes; unpack interleaves within lanes and
 * permute2x128 stitches the four half-lanes into linear order. */
__attribute__((target("avx2,fma"))) static void
simd_avx2_hex_encode(const uint8_t *restrict src, size_t n,
                     char *restrict dst) {
  const __m128i lut128 = _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
  const __m256i lut = _mm256_broadcastsi128_si256(lut128);
  const __m256i lomask = _mm256_set1_epi8(0x0F);
  size_t i = 0, o = 0;
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), lomask);
    __m256i lo = _mm256_and_si256(v, lomask);
    __m256i hc = _mm256_shuffle_epi8(lut, hi);
    __m256i lc = _mm256_shuffle_epi8(lut, lo);
    __m256i a = _mm256_unpacklo_epi8(hc, lc); /* lanes: [0..7][16..23] */
    __m256i b = _mm256_unpackhi_epi8(hc, lc); /* lanes: [8..15][24..31] */
    __m256i o0 = _mm256_permute2x128_si256(a, b, 0x20); /* out[0:32]  */
    __m256i o1 = _mm256_permute2x128_si256(a, b, 0x31); /* out[32:64] */
    _mm256_storeu_si256((__m256i *)(dst + o), o0);
    _mm256_storeu_si256((__m256i *)(dst + o + 32), o1);
    o += 64;
  }
  for (; i < n; i++) {
    dst[o++] = simd_avx2_hexc[src[i] >> 4];
    dst[o++] = simd_avx2_hexc[src[i] & 0x0F];
  }
}

__attribute__((target("avx2,fma"))) static inline __m256i
simd_avx2_in_range(__m256i c, __m256i lo, __m256i hi) {
  return _mm256_cmpeq_epi8(_mm256_max_epu8(_mm256_min_epu8(c, hi), lo), c);
}

/* Decode 32 chars -> 16 bytes: validate, arithmetic nibble select, pack
 * adjacent pairs, then permute the packus qwords back into linear order. */
__attribute__((target("avx2,fma"))) static size_t
simd_avx2_hex_decode(const char *restrict src, size_t n,
                     uint8_t *restrict dst) {
  const __m256i v0 = _mm256_set1_epi8('0'), v9 = _mm256_set1_epi8('9');
  const __m256i va = _mm256_set1_epi8('a'), vf = _mm256_set1_epi8('f');
  const __m256i vA = _mm256_set1_epi8('A'), vF = _mm256_set1_epi8('F');
  const __m256i sd = _mm256_set1_epi8('0');
  const __m256i sl = _mm256_set1_epi8((char)('a' - 10));
  const __m256i su = _mm256_set1_epi8((char)('A' - 10));
  size_t i = 0, o = 0;
  if (n & 1) return SIZE_MAX;
  for (; i + 32 <= n; i += 32) {
    __m256i c = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i isd = simd_avx2_in_range(c, v0, v9);
    __m256i isl = simd_avx2_in_range(c, va, vf);
    __m256i isu = simd_avx2_in_range(c, vA, vF);
    __m256i valid = _mm256_or_si256(isd, _mm256_or_si256(isl, isu));
    __m256i nib, lonib, combined, packed, perm;
    if ((unsigned)_mm256_movemask_epi8(valid) != 0xFFFFFFFFu) return SIZE_MAX;
    nib = _mm256_or_si256(
        _mm256_and_si256(_mm256_sub_epi8(c, sd), isd),
        _mm256_or_si256(_mm256_and_si256(_mm256_sub_epi8(c, sl), isl),
                        _mm256_and_si256(_mm256_sub_epi8(c, su), isu)));
    lonib = _mm256_and_si256(nib, _mm256_set1_epi16(0x00FF));
    combined = _mm256_or_si256(_mm256_slli_epi16(lonib, 4),
                               _mm256_srli_epi16(nib, 8));
    /* packus is per-128-lane: result qwords q0=out[0:8], q2=out[8:16] */
    packed = _mm256_packus_epi16(combined, _mm256_setzero_si256());
    perm = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
    _mm_storeu_si128((__m128i *)(dst + o), _mm256_castsi256_si128(perm));
    o += 16;
  }
  for (; i < n; i += 2) {
    int hi = simd_avx2_hexval((uint8_t)src[i]);
    int lo = simd_avx2_hexval((uint8_t)src[i + 1]);
    if (hi < 0 || lo < 0) return SIZE_MAX;
    dst[o++] = (uint8_t)((hi << 4) | lo);
  }
  return o;
}

__attribute__((target("avx2,fma"))) static size_t
simd_avx2_latin1_to_utf8(const uint8_t *restrict src, size_t n,
                         uint8_t *restrict dst) {
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 32 <= n) {
      __m256i blk = _mm256_loadu_si256((const __m256i *)(src + i));
      if (_mm256_movemask_epi8(blk) == 0) { /* all ASCII */
        _mm256_storeu_si256((__m256i *)(dst + o), blk);
        i += 32; o += 32;
        continue;
      }
    }
    {
      uint8_t c = src[i++];
      if (c < 0x80) {
        dst[o++] = c;
      } else {
        dst[o++] = (uint8_t)(0xC0 | (c >> 6));
        dst[o++] = (uint8_t)(0x80 | (c & 0x3F));
      }
    }
  }
  return o;
}

__attribute__((target("avx2,fma"))) static int
simd_avx2_utf8_to_latin1(const uint8_t *restrict src, size_t n,
                         uint8_t *restrict dst, size_t *out_len) {
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 32 <= n) {
      __m256i blk = _mm256_loadu_si256((const __m256i *)(src + i));
      if (_mm256_movemask_epi8(blk) == 0) {
        _mm256_storeu_si256((__m256i *)(dst + o), blk);
        i += 32; o += 32;
        continue;
      }
    }
    {
      uint8_t c = src[i];
      if (c < 0x80) { dst[o++] = c; i++; continue; }
      if ((c & 0xE0) == 0xC0) {
        uint8_t c1;
        uint32_t cp;
        if (i + 1 >= n) return -1;
        c1 = src[i + 1];
        if ((c1 & 0xC0) != 0x80) return -1;
        cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
        if (cp < 0x80 || cp > 0xFF) return -1;
        dst[o++] = (uint8_t)cp;
        i += 2;
        continue;
      }
      return -1;
    }
  }
  *out_len = o;
  return 0;
}

__attribute__((target("avx2,fma"))) static size_t
simd_avx2_count_utf8(const uint8_t *restrict p, size_t n) {
  const __m256i c0 = _mm256_set1_epi8((char)0xC0);
  const __m256i c80 = _mm256_set1_epi8((char)0x80);
  size_t i = 0, total = 0;
  for (; i + 32 <= n; i += 32) {
    __m256i blk = _mm256_loadu_si256((const __m256i *)(p + i));
    __m256i iscont = _mm256_cmpeq_epi8(_mm256_and_si256(blk, c0), c80);
    total += 32 - (size_t)__builtin_popcount((unsigned)_mm256_movemask_epi8(iscont));
  }
  for (; i < n; i++)
    if ((p[i] & 0xC0) != 0x80) total++;
  return total;
}

/* ── UTF-8 <-> UTF-16 transcode + validation (AVX2) ──────────────────
 * GATED OFF by default (DYNAJS_SIMD_UTF16_AVX2 undefined): this host's amd64
 * emulation (Rosetta under OrbStack) tops out at SSE4.2 and cannot execute
 * AVX2 faithfully under the differential harness, and qemu-AVX is unreliable
 * here. Per the repo's "do not ship unproven x86 SIMD" rule these stay disabled;
 * simd_init installs SSE4.2 before AVX2 and leaves these four slots un-overridden
 * on AVX2 hardware, so it transparently runs the PROVEN SSE4.2 UTF-16 kernels.
 * They mirror the proven SSE4.2 structure (and the packus+permute lane fixup
 * already proven in simd_avx2_hex_decode); define DYNAJS_SIMD_UTF16_AVX2 once
 * they can be exercised on AVX2-capable emulation/hardware. */
#ifdef DYNAJS_SIMD_UTF16_AVX2
__attribute__((target("avx2,fma"))) static int
simd_avx2_utf8_to_utf16le(const uint8_t *restrict src, size_t n,
                          uint16_t *restrict dst, size_t *out_units) {
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 32 <= n) {
      __m256i blk = _mm256_loadu_si256((const __m256i *)(src + i));
      if (_mm256_movemask_epi8(blk) == 0) { /* all ASCII: zero-extend 32->32 */
        __m256i lo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(blk));
        __m256i hi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(blk, 1));
        _mm256_storeu_si256((__m256i *)(dst + o), lo);
        _mm256_storeu_si256((__m256i *)(dst + o + 16), hi);
        i += 32; o += 32;
        continue;
      }
    }
    {
      uint8_t c = src[i];
      size_t len, j;
      uint32_t cp;
      if (c < 0x80) { dst[o++] = c; i++; continue; }
      if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
      else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
      else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
      else return -1;
      if (i + len > n) return -1;
      for (j = 1; j < len; j++) {
        uint8_t cc = src[i + j];
        if ((cc & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (cc & 0x3F);
      }
      if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
          (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
          (cp >= 0xD800 && cp <= 0xDFFF))
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
  }
  *out_units = o;
  return 0;
}

__attribute__((target("avx2,fma"))) static int
simd_avx2_utf16le_to_utf8(const uint16_t *restrict src, size_t units,
                          uint8_t *restrict dst, size_t *out_len) {
  const __m256i m7f80 = _mm256_set1_epi16((short)0xFF80);
  size_t i = 0, o = 0;
  while (i < units) {
    if (i + 32 <= units) {
      __m256i v0 = _mm256_loadu_si256((const __m256i *)(src + i));
      __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + i + 16));
      if (_mm256_testz_si256(_mm256_or_si256(v0, v1), m7f80)) { /* all < 0x80 */
        /* packus is per-128-lane; permute the qwords back to linear order */
        __m256i packed = _mm256_packus_epi16(v0, v1);
        __m256i perm = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
        _mm256_storeu_si256((__m256i *)(dst + o), perm);
        i += 32; o += 32;
        continue;
      }
    }
    {
      uint32_t cp = src[i];
      if (cp < 0xD800 || cp > 0xDFFF) {
        i++;
      } else if (cp <= 0xDBFF) {
        uint32_t lo;
        if (i + 1 >= units) return -1;
        lo = src[i + 1];
        if (lo < 0xDC00 || lo > 0xDFFF) return -1;
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
  }
  *out_len = o;
  return 0;
}

__attribute__((target("avx2,fma"))) static int
simd_avx2_validate_utf16le(const uint16_t *restrict src, size_t units) {
  const __m256i f800 = _mm256_set1_epi16((short)0xF800);
  const __m256i d800 = _mm256_set1_epi16((short)0xD800);
  size_t i = 0;
  while (i < units) {
    if (i + 16 <= units) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
      __m256i issur = _mm256_cmpeq_epi16(_mm256_and_si256(v, f800), d800);
      if (_mm256_movemask_epi8(issur) == 0) { i += 16; continue; } /* all BMP */
    }
    {
      uint32_t c = src[i];
      if (c < 0xD800 || c > 0xDFFF) { i++; continue; }
      if (c > 0xDBFF) return 0;
      if (i + 1 >= units) return 0;
      if (src[i + 1] < 0xDC00 || src[i + 1] > 0xDFFF) return 0;
      i += 2;
    }
  }
  return 1;
}

__attribute__((target("avx2,fma"))) static size_t
simd_avx2_count_utf16(const uint16_t *restrict src, size_t units) {
  const __m256i fc00 = _mm256_set1_epi16((short)0xFC00);
  const __m256i dc00 = _mm256_set1_epi16((short)0xDC00);
  size_t i = 0, total = 0;
  for (; i + 16 <= units; i += 16) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i islow = _mm256_cmpeq_epi16(_mm256_and_si256(v, fc00), dc00);
    total += 16 - (size_t)(__builtin_popcount((unsigned)_mm256_movemask_epi8(islow)) / 2);
  }
  for (; i < units; i++)
    if (src[i] < 0xDC00 || src[i] > 0xDFFF) total++;
  return total;
}
#endif /* DYNAJS_SIMD_UTF16_AVX2 */

/* ── Override table ─────────────────────────────────────────────── */
/* ── Double-precision (f64) kernels — __m256d, 4 lanes (AVX). ────────
 * Horizontal reduces: fold the 256-bit vector to 128 (add/max/min), then the
 * two remaining lanes. Reductions run only full 4-lane bodies then a scalar
 * tail; max/min guard the seed load for n < 4. */
__attribute__((target("avx2,fma"))) static inline double
hsum256_pd(__m256d v) {
  __m128d s = _mm_add_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
  return _mm_cvtsd_f64(_mm_add_sd(s, _mm_unpackhi_pd(s, s)));
}
__attribute__((target("avx2,fma"))) static inline double
hmax256_pd(__m256d v) {
  __m128d s = _mm_max_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
  return _mm_cvtsd_f64(_mm_max_sd(s, _mm_unpackhi_pd(s, s)));
}
__attribute__((target("avx2,fma"))) static inline double
hmin256_pd(__m256d v) {
  __m128d s = _mm_min_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
  return _mm_cvtsd_f64(_mm_min_sd(s, _mm_unpackhi_pd(s, s)));
}

__attribute__((target("avx2,fma"))) static double
simd_avx2_f64_sum(const double *restrict x, size_t n) {
  __m256d acc = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    acc = _mm256_add_pd(acc, _mm256_loadu_pd(&x[i]));
  double result = hsum256_pd(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static double
simd_avx2_f64_dot(const double *restrict a, const double *restrict b,
                  size_t n) {
  __m256d acc = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    acc = _mm256_fmadd_pd(_mm256_loadu_pd(&a[i]), _mm256_loadu_pd(&b[i]), acc);
  double result = hsum256_pd(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

__attribute__((target("avx2,fma"))) static double
simd_avx2_f64_max(const double *restrict x, size_t n) {
  if (n == 0)
    return -DBL_MAX;
  double result;
  size_t i;
  if (n >= 4) {
    __m256d vmax = _mm256_loadu_pd(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm256_max_pd(vmax, _mm256_loadu_pd(&x[i]));
    result = hmax256_pd(vmax);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static double
simd_avx2_f64_min(const double *restrict x, size_t n) {
  if (n == 0)
    return DBL_MAX;
  double result;
  size_t i;
  if (n >= 4) {
    __m256d vmin = _mm256_loadu_pd(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmin = _mm256_min_pd(vmin, _mm256_loadu_pd(&x[i]));
    result = hmin256_pd(vmin);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_f64_scale(double *restrict out, const double *restrict x, double s,
                    size_t n) {
  __m256d vs = _mm256_set1_pd(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm256_storeu_pd(&out[i], _mm256_mul_pd(_mm256_loadu_pd(&x[i]), vs));
  for (; i < n; i++)
    out[i] = x[i] * s;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_f64_axpy(double *restrict y, double a, const double *restrict x,
                   size_t n) {
  __m256d va = _mm256_set1_pd(a);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    /* non-fused mul-then-add (NOT fmadd): bit-exact vs scalar/JS y[i]+=a*x[i] */
    __m256d p = _mm256_mul_pd(va, _mm256_loadu_pd(&x[i]));
    _mm256_storeu_pd(&y[i], _mm256_add_pd(_mm256_loadu_pd(&y[i]), p));
  }
  for (; i < n; i++) {
    double p = a * x[i];
    y[i] = y[i] + p;
  }
}

/* ── Signed 32-bit integer (i32) + prefix-scan kernels — __m256i, 8 lanes.
 * GATED OFF by default (DYNAJS_SIMD_INT_AVX2 undefined): this host's qemu/
 * Rosetta amd64 emulation cannot execute AVX2 reliably, so these mirror the
 * proven SSE4.2 logic (widened to 8 lanes, with a cross-128-lane carry for the
 * prefix scans) and are COMPILE-checked but NOT runtime-proven under the
 * differential harness. Per the repo's "do not ship unproven x86 SIMD" rule they
 * stay disabled; AVX2 hardware therefore runs the PROVEN SSE4.2 i32 kernels
 * (simd_override_sse42 installs them first and this override leaves the slots
 * untouched). Define DYNAJS_SIMD_INT_AVX2 to exercise them on real AVX2 via
 * tests/test_simd_int.c. */
#ifdef DYNAJS_SIMD_INT_AVX2
__attribute__((target("avx2,fma"))) static int64_t
simd_avx2_i32_sum(const int32_t *restrict x, size_t n) {
  __m256i acc = _mm256_setzero_si256(); /* 4 x int64 */
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(x + i));
    acc = _mm256_add_epi64(acc,
                           _mm256_cvtepi32_epi64(_mm256_castsi256_si128(v)));
    acc = _mm256_add_epi64(
        acc, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(v, 1)));
  }
  __m128i s = _mm_add_epi64(_mm256_castsi256_si128(acc),
                            _mm256_extracti128_si256(acc, 1));
  int64_t result =
      (int64_t)_mm_cvtsi128_si64(s) + (int64_t)_mm_extract_epi64(s, 1);
  for (; i < n; i++)
    result += (int64_t)x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static int simd_avx2_hmin_epi32(__m256i v) {
  __m128i r = _mm_min_epi32(_mm256_castsi256_si128(v),
                            _mm256_extracti128_si256(v, 1));
  r = _mm_min_epi32(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(2, 3, 0, 1)));
  r = _mm_min_epi32(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(1, 0, 3, 2)));
  return _mm_cvtsi128_si32(r);
}
__attribute__((target("avx2,fma"))) static int simd_avx2_hmax_epi32(__m256i v) {
  __m128i r = _mm_max_epi32(_mm256_castsi256_si128(v),
                            _mm256_extracti128_si256(v, 1));
  r = _mm_max_epi32(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(2, 3, 0, 1)));
  r = _mm_max_epi32(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(1, 0, 3, 2)));
  return _mm_cvtsi128_si32(r);
}

__attribute__((target("avx2,fma"))) static int32_t
simd_avx2_i32_min(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MAX;
  int32_t result;
  size_t i;
  if (n >= 8) {
    __m256i vmin = _mm256_loadu_si256((const __m256i *)x);
    for (i = 8; i + 8 <= n; i += 8)
      vmin = _mm256_min_epi32(vmin, _mm256_loadu_si256((const __m256i *)(x + i)));
    result = simd_avx2_hmin_epi32(vmin);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static int32_t
simd_avx2_i32_max(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MIN;
  int32_t result;
  size_t i;
  if (n >= 8) {
    __m256i vmax = _mm256_loadu_si256((const __m256i *)x);
    for (i = 8; i + 8 <= n; i += 8)
      vmax = _mm256_max_epi32(vmax, _mm256_loadu_si256((const __m256i *)(x + i)));
    result = simd_avx2_hmax_epi32(vmax);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("avx2,fma"))) static double
simd_avx2_i32_dot(const int32_t *restrict a, const int32_t *restrict b,
                  size_t n) {
  __m256d acc = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i va = _mm256_loadu_si256((const __m256i *)(a + i));
    __m256i vb = _mm256_loadu_si256((const __m256i *)(b + i));
    acc = _mm256_fmadd_pd(_mm256_cvtepi32_pd(_mm256_castsi256_si128(va)),
                          _mm256_cvtepi32_pd(_mm256_castsi256_si128(vb)), acc);
    acc = _mm256_fmadd_pd(_mm256_cvtepi32_pd(_mm256_extracti128_si256(va, 1)),
                          _mm256_cvtepi32_pd(_mm256_extracti128_si256(vb, 1)),
                          acc);
  }
  double result = hsum256_pd(acc);
  for (; i < n; i++)
    result += (double)a[i] * (double)b[i];
  return result;
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_i32_add(int32_t *restrict out, const int32_t *restrict a,
                  const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    _mm256_storeu_si256(
        (__m256i *)(out + i),
        _mm256_add_epi32(_mm256_loadu_si256((const __m256i *)(a + i)),
                         _mm256_loadu_si256((const __m256i *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_i32_mul(int32_t *restrict out, const int32_t *restrict a,
                  const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    _mm256_storeu_si256(
        (__m256i *)(out + i),
        _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(a + i)),
                           _mm256_loadu_si256((const __m256i *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_i32_scale(int32_t *restrict out, const int32_t *restrict x, int32_t s,
                    size_t n) {
  __m256i vs = _mm256_set1_epi32(s);
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    _mm256_storeu_si256(
        (__m256i *)(out + i),
        _mm256_mullo_epi32(_mm256_loadu_si256((const __m256i *)(x + i)), vs));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}

/* Prefix scans: _mm256_slli_si256 prefixes each 128-bit half independently, then
 * the low half's running total (lane 3) is folded into the high 4 lanes. */
__attribute__((target("avx2,fma"))) static void
simd_avx2_f32_cumsum(float *out, const float *x, size_t n) {
  float carry = 0.0f;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 v = _mm256_loadu_ps(x + i);
    v = _mm256_add_ps(
        v, _mm256_castsi256_ps(_mm256_slli_si256(_mm256_castps_si256(v), 4)));
    v = _mm256_add_ps(
        v, _mm256_castsi256_ps(_mm256_slli_si256(_mm256_castps_si256(v), 8)));
    __m128 lo3 = _mm_shuffle_ps(_mm256_castps256_ps128(v),
                                _mm256_castps256_ps128(v), _MM_SHUFFLE(3, 3, 3, 3));
    v = _mm256_add_ps(v, _mm256_insertf128_ps(_mm256_setzero_ps(), lo3, 1));
    v = _mm256_add_ps(v, _mm256_set1_ps(carry));
    _mm256_storeu_ps(out + i, v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    carry = _mm_cvtss_f32(_mm_shuffle_ps(hi, hi, _MM_SHUFFLE(3, 3, 3, 3)));
  }
  for (; i < n; i++) {
    carry += x[i];
    out[i] = carry;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_i32_cumsum(int32_t *out, const int32_t *x, size_t n) {
  int32_t carry = 0;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(x + i));
    v = _mm256_add_epi32(v, _mm256_slli_si256(v, 4));
    v = _mm256_add_epi32(v, _mm256_slli_si256(v, 8));
    __m128i lo3 = _mm_shuffle_epi32(_mm256_castsi256_si128(v),
                                    _MM_SHUFFLE(3, 3, 3, 3));
    v = _mm256_add_epi32(
        v, _mm256_inserti128_si256(_mm256_setzero_si256(), lo3, 1));
    v = _mm256_add_epi32(v, _mm256_set1_epi32(carry));
    _mm256_storeu_si256((__m256i *)(out + i), v);
    carry = _mm_extract_epi32(_mm256_extracti128_si256(v, 1), 3);
  }
  for (; i < n; i++) {
    carry = (int32_t)((uint32_t)carry + (uint32_t)x[i]);
    out[i] = carry;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_f32_cummax(float *out, const float *x, size_t n) {
  const __m256 ninf = _mm256_set1_ps(-INFINITY);
  float carry = -INFINITY;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 v = _mm256_loadu_ps(x + i);
    /* fill the two per-128 low lanes (0,4 then 0,1,4,5) with -inf, not 0 */
    __m256 s1 =
        _mm256_castsi256_ps(_mm256_slli_si256(_mm256_castps_si256(v), 4));
    v = _mm256_max_ps(v, _mm256_blend_ps(s1, ninf, 0x11));
    __m256 s2 =
        _mm256_castsi256_ps(_mm256_slli_si256(_mm256_castps_si256(v), 8));
    v = _mm256_max_ps(v, _mm256_blend_ps(s2, ninf, 0x33));
    __m128 lo3 = _mm_shuffle_ps(_mm256_castps256_ps128(v),
                                _mm256_castps256_ps128(v), _MM_SHUFFLE(3, 3, 3, 3));
    v = _mm256_max_ps(v, _mm256_insertf128_ps(ninf, lo3, 1));
    v = _mm256_max_ps(v, _mm256_set1_ps(carry));
    _mm256_storeu_ps(out + i, v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    carry = _mm_cvtss_f32(_mm_shuffle_ps(hi, hi, _MM_SHUFFLE(3, 3, 3, 3)));
  }
  for (; i < n; i++) {
    if (x[i] > carry)
      carry = x[i];
    out[i] = carry;
  }
}

__attribute__((target("avx2,fma"))) static void
simd_avx2_i32_cummax(int32_t *out, const int32_t *x, size_t n) {
  const __m256i imin = _mm256_set1_epi32(INT32_MIN);
  int32_t carry = INT32_MIN;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(x + i));
    v = _mm256_max_epi32(v, _mm256_blend_epi32(_mm256_slli_si256(v, 4), imin,
                                               0x11));
    v = _mm256_max_epi32(v, _mm256_blend_epi32(_mm256_slli_si256(v, 8), imin,
                                               0x33));
    __m128i lo3 = _mm_shuffle_epi32(_mm256_castsi256_si128(v),
                                    _MM_SHUFFLE(3, 3, 3, 3));
    v = _mm256_max_epi32(v, _mm256_inserti128_si256(imin, lo3, 1));
    v = _mm256_max_epi32(v, _mm256_set1_epi32(carry));
    _mm256_storeu_si256((__m256i *)(out + i), v);
    carry = _mm_extract_epi32(_mm256_extracti128_si256(v, 1), 3);
  }
  for (; i < n; i++) {
    if (x[i] > carry)
      carry = x[i];
    out[i] = carry;
  }
}
#endif /* DYNAJS_SIMD_INT_AVX2 */

/* 256-bit membership, 32 bytes per iteration. vpshufb is per-128-bit lane, so
 * each 16-byte bitmap half is broadcast to both lanes with set_m128i. */
__attribute__((target("avx2")))
static size_t simd_avx2_find_bitmap(const uint8_t *restrict p, size_t n,
                                    const uint8_t *restrict bitmap) {
  const __m128i lo128 = _mm_loadu_si128((const __m128i *)bitmap);
  const __m128i hi128 = _mm_loadu_si128((const __m128i *)(bitmap + 16));
  const __m256i lo_tbl = _mm256_set_m128i(lo128, lo128);
  const __m256i hi_tbl = _mm256_set_m128i(hi128, hi128);
  const __m128i p128 = _mm_setr_epi8(1,2,4,8,16,32,64,(char)128,0,0,0,0,0,0,0,0);
  const __m256i pow2 = _mm256_set_m128i(p128, p128);
  const __m256i m0f = _mm256_set1_epi8(0x0f);
  const __m256i m07 = _mm256_set1_epi8(0x07);
  size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(p + i));
    __m256i idx = _mm256_and_si256(_mm256_srli_epi16(v, 3), m0f);
    __m256i ml = _mm256_shuffle_epi8(lo_tbl, idx);
    __m256i mh = _mm256_shuffle_epi8(hi_tbl, idx);
    __m256i sel = _mm256_blendv_epi8(ml, mh, v);   /* selects on the sign bit */
    __m256i bit = _mm256_shuffle_epi8(pow2, _mm256_and_si256(v, m07));
    __m256i hit = _mm256_and_si256(sel, bit);
    unsigned mask = (unsigned)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(hit, _mm256_setzero_si256()));
    mask = ~mask;
    if (mask) return i + (size_t)__builtin_ctz(mask);
  }
  for (; i < n; i++)
    if (bitmap[p[i] >> 3] & (uint8_t)(1u << (p[i] & 7))) return i;
  return SIZE_MAX;
}

void simd_override_avx2(simd_t *t) {
  t->find_bitmap = simd_avx2_find_bitmap;
  t->hex_encode = simd_avx2_hex_encode;
  t->hex_decode = simd_avx2_hex_decode;
  t->latin1_to_utf8 = simd_avx2_latin1_to_utf8;
  t->utf8_to_latin1 = simd_avx2_utf8_to_latin1;
  t->count_utf8 = simd_avx2_count_utf8;
#ifdef DYNAJS_SIMD_UTF16_AVX2 /* gated OFF: unverifiable here; SSE4.2 runs instead */
  t->utf8_to_utf16le = simd_avx2_utf8_to_utf16le;
  t->utf16le_to_utf8 = simd_avx2_utf16le_to_utf8;
  t->validate_utf16le = simd_avx2_validate_utf16le;
  t->count_utf16 = simd_avx2_count_utf16;
#endif
  t->strfind = simd_avx2_strfind;
  t->dot = simd_avx2_dot;
  t->dot_f = simd_avx2_dot_f;
  t->norm_l2_sq = simd_avx2_norm_l2_sq;
  t->norm_l2 = simd_avx2_norm_l2;
  t->norm_l1 = simd_avx2_norm_l1;
  t->axpy = simd_scalar_axpy; /* AVX2 can do this but scalar is fine */
  t->axpby = simd_scalar_axpby;
  t->add = simd_avx2_add;
  t->sub = simd_avx2_sub;
  t->mul = simd_avx2_mul;
  t->div = simd_avx2_div;
  t->abs = simd_avx2_abs;
  t->fma = simd_avx2_fma_krn;
  t->add_s = simd_avx2_add_s;
  t->mul_s = simd_avx2_mul_s;
  t->scale_add_s = simd_avx2_scale_add_s;
  t->sum = simd_avx2_sum;
  t->max = simd_avx2_max;
  t->min = simd_avx2_min;
  t->argmax = simd_avx2_argmax;
  t->argmin = simd_avx2_argmin;
  t->argminmax = simd_avx2_argminmax;
  t->sigmoid = simd_avx2_sigmoid;
  t->relu = simd_avx2_relu;
  t->relu6 = simd_avx2_relu6;
  t->leaky_relu = simd_avx2_leaky_relu;
  t->elu = simd_avx2_elu;
  t->tanh_fast = simd_avx2_tanh_fast;
  t->gelu = simd_avx2_gelu;
  t->silu = simd_avx2_silu;
  t->softmax = simd_avx2_softmax;
  t->log_softmax = simd_avx2_log_softmax;
  t->vexp = simd_avx2_vexp;
  t->vlog = simd_avx2_vlog;
  t->vsqrt = simd_avx2_vsqrt;
  t->vrsqrt = simd_avx2_vrsqrt;
  t->vinv = simd_avx2_vinv;
  t->dist_l2_sq = simd_avx2_dist_l2_sq;
  t->dist_l1 = simd_avx2_dist_l1;
  t->dist_cos = simd_avx2_dist_cos;
  t->dist_cheb = simd_avx2_dist_cheb;
  t->dist_matrix_l2_sq = simd_avx2_dist_matrix_l2_sq;
  t->dist_matrix_cos = simd_avx2_dist_matrix_cos;
  t->dist_matrix_l1 = simd_avx2_dist_matrix_l1;
  t->gemv = simd_avx2_gemv;
  t->gemv_t = simd_avx2_gemv_t;
  t->gemm = simd_avx2_gemm;
  t->threshold = simd_avx2_threshold;
  t->threshold_sign = simd_avx2_threshold_sign;
  t->hamming = simd_avx2_hamming;
  t->topk_indices = simd_avx2_topk_indices;
  t->clamp = simd_avx2_clamp;
  t->f64_sum = simd_avx2_f64_sum;
  t->f64_dot = simd_avx2_f64_dot;
  t->f64_min = simd_avx2_f64_min;
  t->f64_max = simd_avx2_f64_max;
  t->f64_scale = simd_avx2_f64_scale;
  t->f64_axpy = simd_avx2_f64_axpy;
#ifdef DYNAJS_SIMD_INT_AVX2
  /* Off by default — see the gate comment above. AVX2 HW otherwise keeps the
   * PROVEN SSE4.2 i32/scan kernels installed by simd_override_sse42. */
  t->i32_sum = simd_avx2_i32_sum;
  t->i32_min = simd_avx2_i32_min;
  t->i32_max = simd_avx2_i32_max;
  t->i32_dot = simd_avx2_i32_dot;
  t->i32_add = simd_avx2_i32_add;
  t->i32_mul = simd_avx2_i32_mul;
  t->i32_scale = simd_avx2_i32_scale;
  t->f32_cumsum = simd_avx2_f32_cumsum;
  t->i32_cumsum = simd_avx2_i32_cumsum;
  t->f32_cummax = simd_avx2_f32_cummax;
  t->i32_cummax = simd_avx2_i32_cummax;
#endif
}

#else /* !x86_64 */
void simd_override_avx2(simd_t *t) { (void)t; }
#endif /* x86_64 */