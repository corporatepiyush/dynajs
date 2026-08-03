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

/* SSE4.2 SIMD overrides — 128-bit XMM registers, 4 floats per vector.
 * Compiled only on x86-64 with -msse4.2 via target attribute.
 * Covers the most common BLAS-1 kernels as a bridge between scalar and AVX2. */

/* Whole-file guard: pairs with the #else/#endif stub at the bottom. A
 * stray early #endif here used to orphan that #else, so this file never
 * compiled on any host. */
#if defined(__x86_64__) || defined(_M_X64)
#include <smmintrin.h>
#include <math.h>
#include <string.h>
#include "dyna-simd-kernels.h"
#include <math.h>

/* Horizontal sum of 4 lanes. */
__attribute__((target("sse4.2"))) static inline float
simd_hsum_128(__m128 v) {
  __m128 t = _mm_add_ps(v, _mm_movehl_ps(v, v));
  t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
  return _mm_cvtss_f32(t);
}

/* ── Dot product (float accumulator) ─────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dot(const float *restrict a, const float *restrict b,
                      size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    acc = _mm_add_ps(acc, _mm_mul_ps(va, vb));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

/* ── Dot product (double accumulator for stability) ──────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dot_f(const float *restrict a,
                        const float *restrict b, size_t n) {
  __m128d acc0 = _mm_setzero_pd();
  __m128d acc1 = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 lo = _mm_mul_ps(va, vb);
    acc0 = _mm_add_pd(acc0, _mm_cvtps_pd(lo));
    __m128 hi = _mm_movehl_ps(lo, lo);
    acc1 = _mm_add_pd(acc1, _mm_cvtps_pd(hi));
  }
  double result = acc0[0] + acc0[1] + acc1[0] + acc1[1];
  for (; i < n; i++)
    result += (double)a[i] * b[i];
  return (float)result;
}

/* ── Norm L2 squared ────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l2_sq(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, _mm_mul_ps(vx, vx));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += x[i] * x[i];
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_sse42_norm_l2_sq(x, n));
}

/* ── Norm L1 ─────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l1(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, _mm_andnot_ps(sign_mask, vx));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += fabsf(x[i]);
  return result;
}

/* ── Sum ─────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_sum(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, vx);
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

/* ── Max ─────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_max(const float *restrict x, size_t n) {
  if (n == 0)
    return -FLT_MAX;
  /* The 4-wide head load is only safe when n >= 4. The old reduction ran
   * simd_hsum_128 (a horizontal SUM) over the lane maxes — not a max at all
   * (it returned ~2*(max_ab+max_cd)). Use a real horizontal max (movehl +
   * max_ss), matching NEON's vmaxvq_f32. */
  float result;
  size_t i;
  if (n >= 4) {
    __m128 vmax = _mm_loadu_ps(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_ps(vmax, _mm_loadu_ps(&x[i]));
    __m128 h = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
    h = _mm_max_ss(h, _mm_shuffle_ps(h, h, 1));
    result = _mm_cvtss_f32(h);
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
__attribute__((target("sse4.2"))) static float
simd_sse42_min(const float *restrict x, size_t n) {
  if (n == 0)
    return FLT_MAX;
  /* See simd_sse42_max: the old simd_hsum_128 reduction was a SUM, not a min.
   * Real horizontal min (movehl + min_ss); guard the head load for n < 4. */
  float result;
  size_t i;
  if (n >= 4) {
    __m128 vmin = _mm_loadu_ps(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmin = _mm_min_ps(vmin, _mm_loadu_ps(&x[i]));
    __m128 h = _mm_min_ps(vmin, _mm_movehl_ps(vmin, vmin));
    h = _mm_min_ss(h, _mm_shuffle_ps(h, h, 1));
    result = _mm_cvtss_f32(h);
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
__attribute__((target("sse4.2"))) static size_t
simd_sse42_argmax(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m128 vmax = _mm_loadu_ps(x);
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_max = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask = _mm_cmpgt_ps(vi, vmax);
    vmax = _mm_max_ps(vmax, vi);
    vidx_max = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(vidx_max),
                                              _mm_castsi128_ps(vidxi), mask));
  }
  float tmp[4];
  uint32_t idx_tmp[4];
  _mm_storeu_ps(tmp, vmax);
  _mm_storeu_si128((__m128i *)idx_tmp, vidx_max);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 4; k++)
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
__attribute__((target("sse4.2"))) static size_t
simd_sse42_argmin(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m128 vmin = _mm_loadu_ps(x);
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_min = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask = _mm_cmplt_ps(vi, vmin);
    vmin = _mm_min_ps(vmin, vi);
    vidx_min = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(vidx_min),
                                              _mm_castsi128_ps(vidxi), mask));
  }
  float tmp[4];
  uint32_t idx_tmp[4];
  _mm_storeu_ps(tmp, vmin);
  _mm_storeu_si128((__m128i *)idx_tmp, vidx_min);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 4; k++)
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
__attribute__((target("sse4.2"))) static void
simd_sse42_argminmax(const float *restrict x, size_t n,
                            size_t *argmin_out, size_t *argmax_out) {
  if (n == 0) {
    *argmin_out = *argmax_out = 0;
    return;
  }
  __m128 vmin = _mm_loadu_ps(x);
  __m128 vmax = vmin;
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_min = vidx, vidx_max = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask_lt = _mm_cmplt_ps(vi, vmin);
    __m128 mask_gt = _mm_cmpgt_ps(vi, vmax);
    vmin = _mm_min_ps(vmin, vi);
    vmax = _mm_max_ps(vmax, vi);
    vidx_min = _mm_castps_si128(_mm_blendv_ps(
        _mm_castsi128_ps(vidx_min), _mm_castsi128_ps(vidxi), mask_lt));
    vidx_max = _mm_castps_si128(_mm_blendv_ps(
        _mm_castsi128_ps(vidx_max), _mm_castsi128_ps(vidxi), mask_gt));
  }
  float tmp_min[4], tmp_max[4];
  uint32_t idx_min[4], idx_max[4];
  _mm_storeu_ps(tmp_min, vmin);
  _mm_storeu_ps(tmp_max, vmax);
  _mm_storeu_si128((__m128i *)idx_min, vidx_min);
  _mm_storeu_si128((__m128i *)idx_max, vidx_max);
  float best_min = tmp_min[0], best_max = tmp_max[0];
  size_t imin = idx_min[0], imax = idx_max[0];
  for (size_t k = 1; k < 4; k++) {
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
__attribute__((target("sse4.2"))) static void
simd_sse42_add(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_add_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] + b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_sub(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_sub_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] - b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_mul(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_mul_ps(_mm_loadu_ps(&a[i]), _mm_loadu_ps(&b[i])));
  }
  for (; i < n; i++)
    z[i] = a[i] * b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_div(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_div_ps(_mm_loadu_ps(&a[i]), _mm_loadu_ps(&b[i])));
  }
  for (; i < n; i++)
    z[i] = a[i] / b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_abs(float *restrict out, const float *restrict in,
                      size_t n) {
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_andnot_ps(sign_mask, vi));
  }
  for (; i < n; i++)
    out[i] = fabsf(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_fma(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vz = _mm_loadu_ps(&z[i]);
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_mul_ps(va, vb), vz));
  }
  for (; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ──────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_add_s(float *restrict z, const float *restrict x,
                        float s, size_t n) {
  __m128 vs = _mm_set1_ps(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_loadu_ps(&x[i]), vs));
  }
  for (; i < n; i++)
    z[i] = x[i] + s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_mul_s(float *restrict z, const float *restrict x,
                        float s, size_t n) {
  __m128 vs = _mm_set1_ps(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_mul_ps(_mm_loadu_ps(&x[i]), vs));
  }
  for (; i < n; i++)
    z[i] = x[i] * s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_scale_add_s(float *restrict z, float alpha,
                              const float *restrict x, float beta,
                              size_t n) {
  __m128 va = _mm_set1_ps(alpha);
  __m128 vb = _mm_set1_ps(beta);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_mul_ps(va, _mm_loadu_ps(&x[i])), vb));
  }
  for (; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ──────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_sigmoid(float *restrict out, const float *restrict in,
                          size_t n) {
  __m128 one = _mm_set1_ps(1.0f);
  __m128 lo = _mm_set1_ps(-30.0f);
  __m128 hi = _mm_set1_ps(30.0f);
  __m128 magic = _mm_set1_ps(-12102203.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128i zero = _mm_setzero_si128();
  __m128i top = _mm_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    vi = _mm_max_ps(_mm_min_ps(vi, hi), lo);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, zero);
    bits = _mm_min_epi32(bits, top);
    __m128 ve = _mm_add_ps(one, _mm_castsi128_ps(bits));
    _mm_storeu_ps(&out[i], _mm_div_ps(one, ve));
  }
  for (; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_tanh(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 two = _mm_set1_ps(2.0f);
  __m128 one = _mm_set1_ps(1.0f);
  __m128 lo = _mm_set1_ps(-10.0f);
  __m128 hi = _mm_set1_ps(10.0f);
  __m128 magic = _mm_set1_ps(-12102203.0f * 2.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128i zero = _mm_setzero_si128();
  __m128i top = _mm_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    vi = _mm_max_ps(_mm_min_ps(vi, hi), lo);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, zero);
    bits = _mm_min_epi32(bits, top);
    __m128 ve = _mm_sub_ps(
        _mm_div_ps(two, _mm_add_ps(one, _mm_castsi128_ps(bits))), one);
    _mm_storeu_ps(&out[i], ve);
  }
  for (; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_gelu(float *restrict out, const float *restrict in,
                       size_t n) {
  const __m128 sqrt_2_over_pi = _mm_set1_ps(0.7978845608028654f);
  const __m128 c = _mm_set1_ps(0.044715f);
  const __m128 half = _mm_set1_ps(0.5f);
  const __m128 one = _mm_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 x = _mm_loadu_ps(&in[i]);
    __m128 x3 = _mm_mul_ps(_mm_mul_ps(x, x), x);
    __m128 inner = _mm_mul_ps(sqrt_2_over_pi, _mm_add_ps(x, _mm_mul_ps(c, x3)));
    /* _mm_tanh_ps is SVML-only; per-lane libm tanhf keeps the lanes
     * bit-identical to the scalar tail. The bit-exp trick tanh
     * (2*sigmoid(2y)-1) used here before was the fast_tanh approximation
     * (~34% error) — gelu is an accurate activation and needs the real tanh. */
    {
      float tmp[4];
      _mm_storeu_ps(tmp, inner);
      for (int j = 0; j < 4; j++)
        tmp[j] = tanhf(tmp[j]);
      inner = _mm_loadu_ps(tmp);
    }
    _mm_storeu_ps(&out[i],
                  _mm_mul_ps(_mm_mul_ps(half, x), _mm_add_ps(one, inner)));
  }
  for (; i < n; i++) {
    float x = in[i];
    float x3 = x * x * x;
    float inner = tanhf(0.7978845608028654f * (x + 0.044715f * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_relu(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 zero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_max_ps(vi, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_relu6(float *restrict out, const float *restrict in,
                        size_t n) {
  __m128 zero = _mm_setzero_ps();
  __m128 six = _mm_set1_ps(6.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_min_ps(_mm_max_ps(vi, zero), six));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_leaky_relu(float *restrict out,
                             const float *restrict in, float slope,
                             size_t n) {
  __m128 vzero = _mm_setzero_ps();
  __m128 vslope = _mm_set1_ps(slope);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vzero);
    __m128 res = _mm_add_ps(_mm_and_ps(mask, vi),
                            _mm_andnot_ps(mask, _mm_mul_ps(vi, vslope)));
    _mm_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_elu(float *restrict out, const float *restrict in,
                      float alpha, size_t n) {
  __m128 vzero = _mm_setzero_ps();
  __m128 valpha = _mm_set1_ps(alpha);
  __m128 vone = _mm_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vzero);
    /* _mm_exp_ps is SVML-only; per-lane libm exp matches the tail. */
    __m128 vexp;
    {
      float tmp[4];
      _mm_storeu_ps(tmp, vi);
      for (int j = 0; j < 4; j++)
        tmp[j] = expf(tmp[j]);
      vexp = _mm_loadu_ps(tmp);
    }
    __m128 exp_part = _mm_sub_ps(vexp, vone);
    __m128 res = _mm_add_ps(_mm_and_ps(mask, vi),
                            _mm_andnot_ps(mask, _mm_mul_ps(valpha, exp_part)));
    _mm_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

/* Vectorized Schraudolph exp, matching fast_exp bit-for-bit:
 * bits = (int)(12102203·x + 0x3F800000), clamped to [0, +inf bits]. */
__attribute__((target("sse4.2"))) static inline __m128
sse42_fast_exp_ps(__m128 x) {
  __m128i bits = _mm_cvtps_epi32(
      _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(12102203.0f)),
                 _mm_set1_ps(1065353216.0f)));
  bits = _mm_max_epi32(bits, _mm_setzero_si128());
  bits = _mm_min_epi32(bits, _mm_set1_epi32(0x7F800000));
  return _mm_castsi128_ps(bits);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_softmax(float *restrict out, const float *restrict in,
                          size_t n) {
  if (unlikely(n == 0))
    return;
  /* Max reduction. The 4-wide head load is only safe when n >= 4 —
   * softmax is routinely called with tiny n (class counts). */
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    __m128 vmax = _mm_loadu_ps(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_ps(vmax, _mm_loadu_ps(&in[i]));
    __m128 shuf = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
    shuf = _mm_max_ss(shuf, _mm_shuffle_ps(shuf, shuf, 1));
    maxv = _mm_cvtss_f32(shuf);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  __m128 vmaxv = _mm_set1_ps(maxv);
  i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i],
                  sse42_fast_exp_ps(_mm_sub_ps(_mm_loadu_ps(&in[i]), vmaxv)));
  for (; i < n; i++)
    out[i] = fast_exp(in[i] - maxv);

  float sum = simd_hsum_f32(out, n);
  float inv_sum = 1.0f / sum;
  __m128 vinv = _mm_set1_ps(inv_sum);
  for (i = 0; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i], _mm_mul_ps(_mm_loadu_ps(&out[i]), vinv));
  for (; i < n; i++)
    out[i] *= inv_sum;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_log_softmax(float *restrict out,
                              const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    __m128 vmax = _mm_loadu_ps(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_ps(vmax, _mm_loadu_ps(&in[i]));
    __m128 shuf = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
    shuf = _mm_max_ss(shuf, _mm_shuffle_ps(shuf, shuf, 1));
    maxv = _mm_cvtss_f32(shuf);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* Accumulate the exp-sum in a register. Storing exps into out[] would
   * clobber in[] when out aliases in (the binding calls log_softmax(a, a, n)),
   * and the final in[i]-voff loop still needs the original input — the old
   * "exps land in out[]" code produced a ~550% error on in-place input. */
  __m128 vmaxv = _mm_set1_ps(maxv);
  __m128 vsum = _mm_setzero_ps();
  i = 0;
  for (; i + 4 <= n; i += 4)
    vsum = _mm_add_ps(
        vsum, sse42_fast_exp_ps(_mm_sub_ps(_mm_loadu_ps(&in[i]), vmaxv)));
  float sum = simd_hsum_128(vsum);
  for (; i < n; i++)
    sum += fast_exp(in[i] - maxv);

  float log_s = logf(sum);
  __m128 voff = _mm_set1_ps(maxv + log_s);
  for (i = 0; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i], _mm_sub_ps(_mm_loadu_ps(&in[i]), voff));
  for (; i < n; i++)
    out[i] = in[i] - maxv - log_s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vexp(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 magic = _mm_set1_ps(12102203.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, _mm_setzero_si128());
    bits = _mm_min_epi32(bits, _mm_set1_epi32(0x7F800000));
    __m128 ve = _mm_castsi128_ps(bits);
    _mm_storeu_ps(&out[i], _mm_max_ps(ve, vzero));
  }
  for (; i < n; i++)
    out[i] = fast_exp(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vlog(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  (void)vzero;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    /* _mm_log_ps is SVML-only; compute per lane with libm logf. */
    __m128 vln = _mm_set1_ps(-FLT_MAX);
    /* Scalar fallback embedded: only compute log for positive lanes */
    for (size_t j = 0; j < 4; j++) {
      float vf = ((float *)&vi)[j];
      ((float *)&vln)[j] = vf > 0.0f ? logf(vf) : -FLT_MAX;
    }
    _mm_storeu_ps(&out[i], vln);
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vsqrt(float *restrict out, const float *restrict in,
                        size_t n) {
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpge_ps(vi, vzero);
    __m128 vs = _mm_sqrt_ps(vi);
    vs = _mm_and_ps(mask, vs);
    _mm_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vrsqrt(float *restrict out, const float *restrict in,
                         size_t n) {
  size_t i = 0;
  const __m128 half = _mm_set1_ps(0.5f), three = _mm_set1_ps(3.0f);
  const __m128 zero = _mm_setzero_ps();
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 vs = _mm_rsqrt_ps(vi);
    /* rsqrtps is an ESTIMATE: 12 bits, relative error up to ~3.7e-4. Raw, it
       is nowhere near the accuracy the NEON kernel delivers (vrsqrte plus TWO
       Newton-Raphson steps) or the exact scalar tail three lines below, so the
       same call returned ~1e-4 accuracy for its first 4k elements and exact
       values for the remainder. Two NR steps here as well:
           y <- y * (3 - x*y*y) / 2
       Each step doubles the correct bits, so 12 -> 24 exceeds what f32 holds. */
    vs = _mm_mul_ps(_mm_mul_ps(half, vs),
                    _mm_sub_ps(three, _mm_mul_ps(vi, _mm_mul_ps(vs, vs))));
    vs = _mm_mul_ps(_mm_mul_ps(half, vs),
                    _mm_sub_ps(three, _mm_mul_ps(vi, _mm_mul_ps(vs, vs))));
    /* x <= 0 has no reciprocal root; the scalar tail answers 0, so match it
       rather than propagating the estimate's inf/NaN. */
    vs = _mm_and_ps(vs, _mm_cmpgt_ps(vi, zero));
    _mm_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vinv(float *restrict out, const float *restrict in,
                       size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_rcp_ps(vi));
  }
  for (; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dist_l2_sq(const float *restrict a,
                             const float *restrict b, size_t d) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 diff = _mm_sub_ps(va, vb);
    acc = _mm_add_ps(acc, _mm_mul_ps(diff, diff));
  }
  float result = simd_hsum_128(acc);
  for (; i < d; i++) {
    float df = a[i] - b[i];
    result += df * df;
  }
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_l1(const float *restrict a,
                          const float *restrict b, size_t d) {
  __m128 acc = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 diff = _mm_sub_ps(va, vb);
    acc = _mm_add_ps(acc, _mm_andnot_ps(sign_mask, diff));
  }
  float result = simd_hsum_128(acc);
  for (; i < d; i++)
    result += fabsf(a[i] - b[i]);
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_cos(const float *restrict a,
                           const float *restrict b, size_t d) {
  __m128 vdot = _mm_setzero_ps();
  __m128 vna = _mm_setzero_ps();
  __m128 vnb = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    vdot = _mm_add_ps(vdot, _mm_mul_ps(va, vb));
    vna = _mm_add_ps(vna, _mm_mul_ps(va, va));
    vnb = _mm_add_ps(vnb, _mm_mul_ps(vb, vb));
  }
  float dot = simd_hsum_128(vdot);
  float na = simd_hsum_128(vna);
  float nb = simd_hsum_128(vnb);
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

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_cheb(const float *restrict a,
                            const float *restrict b, size_t d) {
  __m128 vmax = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 adiff = _mm_andnot_ps(sign_mask, _mm_sub_ps(va, vb));
    vmax = _mm_max_ps(vmax, adiff);
  }
  /* Real horizontal max (Chebyshev = max abs diff); the old simd_hsum_128
   * summed the lane maxes instead. */
  __m128 h = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
  h = _mm_max_ss(h, _mm_shuffle_ps(h, h, 1));
  float result = _mm_cvtss_f32(h);
  for (; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > result)
      result = df;
  }
  return result;
}

/* ── Distance matrix ──────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_dist_matrix_l2_sq(float *restrict out,
                                    const float *restrict a,
                                    const float *restrict b, size_t n,
                                    size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_l2_sq(&a[i * d], &b[j * d], d);
}

__attribute__((target("sse4.2"))) static void simd_sse42_dist_matrix_cos(
    float *restrict out, const float *restrict a,
    const float *restrict b, size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_cos(&a[i * d], &b[j * d], d);
}

__attribute__((target("sse4.2"))) static void simd_sse42_dist_matrix_l1(
    float *restrict out, const float *restrict a,
    const float *restrict b, size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_l1(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: gemv ───────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_gemv(float *restrict y, const float *restrict a,
                       const float *restrict x, size_t m, size_t n,
                       float beta) {
  for (size_t i = 0; i < m; i++) {
    __m128 acc0 = _mm_setzero_ps();
    __m128 acc1 = _mm_setzero_ps();
    size_t j = 0;
    for (; j + 8 <= n; j += 8) {
      acc0 = _mm_add_ps(
          acc0, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j]), _mm_loadu_ps(&x[j])));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j + 4]),
                                         _mm_loadu_ps(&x[j + 4])));
    }
    for (; j + 4 <= n; j += 4)
      acc0 = _mm_add_ps(
          acc0, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j]), _mm_loadu_ps(&x[j])));
    __m128 sum = _mm_add_ps(acc0, acc1);
    float result = simd_hsum_128(sum);
    for (; j < n; j++)
      result += a[i * n + j] * x[j];
    y[i] = beta * y[i] + result;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_gemv_t(float *restrict y, const float *restrict a,
                         const float *restrict x, size_t m, size_t n,
                         float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    __m128 vxi = _mm_set1_ps(x[i]);
    const float *row = &a[i * n];
    size_t j = 0;
    for (; j + 4 <= n; j += 4) {
      __m128 vy = _mm_loadu_ps(&y[j]);
      __m128 va = _mm_loadu_ps(&row[j]);
      _mm_storeu_ps(&y[j], _mm_add_ps(vy, _mm_mul_ps(vxi, va)));
    }
    for (; j < n; j++)
      y[j] += x[i] * row[j];
  }
}

/* ── BLAS Level 3: gemm (tiled) ───────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_gemm(float *restrict c, const float *restrict a,
                       const float *restrict b, size_t m, size_t n,
                       size_t k, float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j + 4 <= n; j += 4) {
      __m128 vc = _mm_loadu_ps(&c[i * n + j]);
      vc = _mm_mul_ps(vc, _mm_set1_ps(beta));
      _mm_storeu_ps(&c[i * n + j], vc);
    }
  for (size_t j = n - (n % 4); j < n; j++)
    for (size_t i = 0; i < m; i++)
      c[i * n + j] *= beta;

  for (size_t i0 = 0; i0 < m; i0 += T) {
    size_t imax = i0 + T < m ? i0 + T : m;
    for (size_t j0 = 0; j0 < n; j0 += T) {
      size_t jmax = j0 + T < n ? j0 + T : n;
      for (size_t k0 = 0; k0 < k; k0 += T) {
        size_t kmax = k0 + T < k ? k0 + T : k;
        /* C tile held in registers across k -- see the NEON kernel. The old
         * form loaded and stored it on every k iteration: two memory ops per
         * multiply-add, where a register-blocked kernel has none. */
        for (size_t i = i0; i < imax; i++) {
          size_t j = j0;
          for (; j + 8 <= jmax; j += 8) {
            __m128 c0 = _mm_loadu_ps(&c[i * n + j]);
            __m128 c1 = _mm_loadu_ps(&c[i * n + j + 4]);
            for (size_t kk = k0; kk < kmax; kk++) {
              __m128 vaik = _mm_set1_ps(alpha * a[i * k + kk]);
              c0 = _mm_add_ps(c0, _mm_mul_ps(vaik, _mm_loadu_ps(&b[kk * n + j])));
              c1 = _mm_add_ps(c1, _mm_mul_ps(vaik, _mm_loadu_ps(&b[kk * n + j + 4])));
            }
            _mm_storeu_ps(&c[i * n + j], c0);
            _mm_storeu_ps(&c[i * n + j + 4], c1);
          }
          for (; j + 4 <= jmax; j += 4) {
            __m128 c0 = _mm_loadu_ps(&c[i * n + j]);
            for (size_t kk = k0; kk < kmax; kk++)
              c0 = _mm_add_ps(c0, _mm_mul_ps(_mm_set1_ps(alpha * a[i * k + kk]),
                                             _mm_loadu_ps(&b[kk * n + j])));
            _mm_storeu_ps(&c[i * n + j], c0);
          }
          for (; j < jmax; j++) {
            float acc = c[i * n + j];
            for (size_t kk = k0; kk < kmax; kk++)
              acc += alpha * a[i * k + kk] * b[kk * n + j];
            c[i * n + j] = acc;
          }
        }
      }
    }
  }
}

/* ── Comparison / Selection ───────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_threshold(float *restrict out,
                            const float *restrict in, float t, size_t n) {
  __m128 vt = _mm_set1_ps(t);
  __m128 one = _mm_set1_ps(1.0f);
  __m128 zero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vt);
    _mm_storeu_ps(&out[i], _mm_blendv_ps(zero, one, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

__attribute__((target("sse4.2"))) static void simd_sse42_threshold_sign(
    float *restrict out, const float *restrict in, float t, size_t n) {
  __m128 vt = _mm_set1_ps(t);
  __m128 pos = _mm_set1_ps(1.0f);
  __m128 neg = _mm_set1_ps(-1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpge_ps(vi, vt);
    _mm_storeu_ps(&out[i], _mm_blendv_ps(neg, pos, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ──────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_hamming(const uint32_t *restrict a,
                          const uint32_t *restrict b, size_t n_words) {
  /* There is no _mm_popcnt_epi32 (vector popcount needs AVX-512
   * VPOPCNTDQ); SSE4.2 provides scalar POPCNT, which the compiler emits
   * for __builtin_popcount under this target. */
  uint32_t result = 0;
  for (size_t i = 0; i < n_words; i++)
    result += (uint32_t)__builtin_popcount(a[i] ^ b[i]);
  return (float)result;
}

/* ── Top-K (uses scalar heap — the vectorized partition is in AVX2) ─ */
__attribute__((target("sse4.2"))) static void
simd_sse42_topk_indices(const float *restrict vals,
                               uint32_t *restrict indices, size_t n,
                               size_t k) {
  /* Fall through to scalar topk */
  simd_scalar_topk_indices(vals, indices, n, k);
}

/* ── Clamp ──────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_clamp(float *restrict out, const float *restrict in,
                        float lo, float hi, size_t n) {
  __m128 vlo = _mm_set1_ps(lo);
  __m128 vhi = _mm_set1_ps(hi);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 clamped = _mm_min_ps(_mm_max_ps(vi, vlo), vhi);
    _mm_storeu_ps(&out[i], clamped);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* Substring search, first+last algorithm over 16-byte blocks (SSE). */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_strfind(const uint8_t *text, size_t n, const uint8_t *pat,
                   size_t m) {
  if (m == 0) return 0;
  if (m > n) return SIZE_MAX;
  if (m == 1) {
    const void *r = memchr(text, pat[0], n);
    return r ? (size_t)((const uint8_t *)r - text) : SIZE_MAX;
  }
  __m128i vfirst = _mm_set1_epi8((char)pat[0]);
  __m128i vlast = _mm_set1_epi8((char)pat[m - 1]);
  size_t i = 0;
  while (i + 16 + (m - 1) <= n) {
    __m128i bf = _mm_loadu_si128((const __m128i *)(text + i));
    __m128i bl = _mm_loadu_si128((const __m128i *)(text + i + m - 1));
    unsigned mask = (unsigned)_mm_movemask_epi8(
        _mm_and_si128(_mm_cmpeq_epi8(bf, vfirst), _mm_cmpeq_epi8(bl, vlast)));
    while (mask) {
      int j = __builtin_ctz(mask);
      if (m == 2 || memcmp(text + i + j + 1, pat + 1, m - 2) == 0)
        return i + j;
      mask &= mask - 1;
    }
    i += 16;
  }
  size_t limit = n - m;
  while (i <= limit) {
    if (text[i] == pat[0] && memcmp(text + i + 1, pat + 1, m - 1) == 0)
      return i;
    i++;
  }
  return SIZE_MAX;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_count_u8(const uint8_t *restrict p, uint8_t v, size_t n) {
  __m128i vv = _mm_set1_epi8((char)v);
  size_t i = 0, total = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i cmp = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), vv);
    total += (size_t)__builtin_popcount((unsigned)_mm_movemask_epi8(cmp));
  }
  for (; i < n; i++)
    if (p[i] == v) total++;
  return total;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_find_first_of(const uint8_t *restrict p, size_t n,
                         const uint8_t *restrict set, size_t setlen) {
  uint8_t tbl[256];
  size_t i = 0, k;
  if (setlen == 0) return SIZE_MAX;
  memset(tbl, 0, sizeof(tbl));
  for (k = 0; k < setlen; k++) tbl[set[k]] = 1;
  if (setlen <= 8) {
    __m128i vset[8];
    for (k = 0; k < setlen; k++) vset[k] = _mm_set1_epi8((char)set[k]);
    for (; i + 16 <= n; i += 16) {
      __m128i blk = _mm_loadu_si128((const __m128i *)(p + i));
      __m128i m = _mm_cmpeq_epi8(blk, vset[0]);
      for (k = 1; k < setlen; k++)
        m = _mm_or_si128(m, _mm_cmpeq_epi8(blk, vset[k]));
      unsigned mask = (unsigned)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
  for (; i < n; i++)
    if (tbl[p[i]]) return i;
  return SIZE_MAX;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_validate_utf8(const uint8_t *restrict p, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (i + 16 <= n) {
      __m128i blk = _mm_loadu_si128((const __m128i *)(p + i));
      if (_mm_movemask_epi8(blk) == 0) { i += 16; continue; } /* all ASCII */
    }
    uint8_t c = p[i];
    size_t len, j;
    uint32_t cp;
    if (c < 0x80) { i++; continue; }
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return i;
    if (i + len > n) return i;
    for (j = 1; j < len; j++) {
      uint8_t cc = p[i + j];
      if ((cc & 0xC0) != 0x80) return i;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return i;
    i += len;
  }
  return n;
}

/* ── hex / latin1 / utf8 byte kernels (SSE4.2 / SSSE3) ───────────── */

static const char simd_sse_hexc[] = "0123456789abcdef";

static inline int simd_sse_hexval(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Encode 16 bytes: PSHUFB nibble->char lookup, then unpack-interleave to
 * h0 l0 h1 l1 ... across two 16-byte stores. */
__attribute__((target("sse4.2"))) static void
simd_sse42_hex_encode(const uint8_t *restrict src, size_t n,
                      char *restrict dst) {
  const __m128i lut = _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
  const __m128i lomask = _mm_set1_epi8(0x0F);
  size_t i = 0, o = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
    __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), lomask);
    __m128i lo = _mm_and_si128(v, lomask);
    __m128i hc = _mm_shuffle_epi8(lut, hi);
    __m128i lc = _mm_shuffle_epi8(lut, lo);
    _mm_storeu_si128((__m128i *)(dst + o), _mm_unpacklo_epi8(hc, lc));
    _mm_storeu_si128((__m128i *)(dst + o + 16), _mm_unpackhi_epi8(hc, lc));
    o += 32;
  }
  for (; i < n; i++) {
    dst[o++] = simd_sse_hexc[src[i] >> 4];
    dst[o++] = simd_sse_hexc[src[i] & 0x0F];
  }
}

/* Unsigned in-range test [lo,hi] via saturating min/max + cmpeq (SSE lacks an
 * unsigned byte compare). */
__attribute__((target("sse4.2"))) static inline __m128i
simd_sse_in_range(__m128i c, __m128i lo, __m128i hi) {
  return _mm_cmpeq_epi8(_mm_max_epu8(_mm_min_epu8(c, hi), lo), c);
}

/* Decode 16 chars -> 8 bytes: validate all lanes, arithmetic nibble select,
 * then pack adjacent nibble pairs (hi<<4)|lo via 16-bit-lane shifts. */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_hex_decode(const char *restrict src, size_t n,
                      uint8_t *restrict dst) {
  /* Case-folded classification: see the NEON kernel. Two subtractions and two
     unsigned range tests replace three ranges and three masked selects. */
  const __m128i sd = _mm_set1_epi8('0');
  const __m128i sl = _mm_set1_epi8((char)('a' - 10));
  const __m128i fold = _mm_set1_epi8(0x20);
  const __m128i n9 = _mm_set1_epi8(9);
  const __m128i n10 = _mm_set1_epi8(10), n15 = _mm_set1_epi8(15);
  size_t i = 0, o = 0;
  __m128i vacc = _mm_set1_epi8((char)0xFF);
  if (n & 1) return SIZE_MAX;
  for (; i + 16 <= n; i += 16) {
    __m128i c = _mm_loadu_si128((const __m128i *)(src + i));
    __m128i dv = _mm_sub_epi8(c, sd);                    /* '0'-'9' -> 0..9   */
    __m128i av = _mm_sub_epi8(_mm_or_si128(c, fold), sl); /* 'a'/'A' -> 10..15 */
    __m128i isd = _mm_cmpeq_epi8(_mm_min_epu8(dv, n9), dv);
    __m128i isa = _mm_and_si128(_mm_cmpeq_epi8(_mm_max_epu8(av, n10), av),
                                _mm_cmpeq_epi8(_mm_min_epu8(av, n15), av));
    __m128i valid = _mm_or_si128(isd, isa);
    __m128i nib, lonib, combined, packed;
    /* accumulated, tested once after the loop -- see the NEON kernel */
    vacc = _mm_and_si128(vacc, valid);
    nib = _mm_or_si128(_mm_and_si128(dv, isd), _mm_andnot_si128(isd, av));
    /* 16-bit lane j = nib[2j] | nib[2j+1]<<8; want low byte = nib[2j]<<4|nib[2j+1] */
    lonib = _mm_and_si128(nib, _mm_set1_epi16(0x00FF));
    combined = _mm_or_si128(_mm_slli_epi16(lonib, 4), _mm_srli_epi16(nib, 8));
    packed = _mm_packus_epi16(combined, _mm_setzero_si128());
    _mm_storel_epi64((__m128i *)(dst + o), packed);
    o += 8;
  }
  if ((unsigned)_mm_movemask_epi8(vacc) != 0xFFFFu) return SIZE_MAX;
  for (; i < n; i += 2) {
    int hi = simd_sse_hexval((uint8_t)src[i]);
    int lo = simd_sse_hexval((uint8_t)src[i + 1]);
    if (hi < 0 || lo < 0) return SIZE_MAX;
    dst[o++] = (uint8_t)((hi << 4) | lo);
  }
  return o;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_latin1_to_utf8(const uint8_t *restrict src, size_t n,
                          uint8_t *restrict dst) {
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 16 <= n) {
      __m128i blk = _mm_loadu_si128((const __m128i *)(src + i));
      if (_mm_movemask_epi8(blk) == 0) { /* all ASCII */
        _mm_storeu_si128((__m128i *)(dst + o), blk);
        i += 16; o += 16;
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

__attribute__((target("sse4.2"))) static int
simd_sse42_utf8_to_latin1(const uint8_t *restrict src, size_t n,
                          uint8_t *restrict dst, size_t *out_len) {
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 16 <= n) {
      __m128i blk = _mm_loadu_si128((const __m128i *)(src + i));
      if (_mm_movemask_epi8(blk) == 0) {
        _mm_storeu_si128((__m128i *)(dst + o), blk);
        i += 16; o += 16;
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

__attribute__((target("sse4.2"))) static size_t
simd_sse42_count_utf8(const uint8_t *restrict p, size_t n) {
  const __m128i c0 = _mm_set1_epi8((char)0xC0), c80 = _mm_set1_epi8((char)0x80);
  size_t i = 0, total = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i blk = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i iscont = _mm_cmpeq_epi8(_mm_and_si128(blk, c0), c80);
    total += 16 - (size_t)__builtin_popcount((unsigned)_mm_movemask_epi8(iscont));
  }
  for (; i < n; i++)
    if ((p[i] & 0xC0) != 0x80) total++;
  return total;
}

/* ── UTF-8 <-> UTF-16 transcode + validation (SSE4.2 / SSE4.1) ───────
 * SIMD fast path for the common case, scalar code point loop otherwise; the
 * scalar path is byte-identical to the scalar reference. */

/* ASCII fast path: a pure-ASCII 16-byte block widens (unpack vs zero) to 16
 * UTF-16 units; anything else falls to the scalar decoder. */
__attribute__((target("sse4.2"))) static int
simd_sse42_utf8_to_utf16le(const uint8_t *restrict src, size_t n,
                           uint16_t *restrict dst, size_t *out_units) {
  const __m128i zero = _mm_setzero_si128();
  size_t i = 0, o = 0;
  while (i < n) {
    if (i + 16 <= n) {
      __m128i blk = _mm_loadu_si128((const __m128i *)(src + i));
      if (_mm_movemask_epi8(blk) == 0) { /* all ASCII */
        _mm_storeu_si128((__m128i *)(dst + o), _mm_unpacklo_epi8(blk, zero));
        _mm_storeu_si128((__m128i *)(dst + o + 8), _mm_unpackhi_epi8(blk, zero));
        i += 16; o += 16;
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

/* Fast path: 16 UTF-16 units all < 0x80 pack (unsigned-saturating, no clamp
 * since < 0x80) to 16 UTF-8 bytes. */
__attribute__((target("sse4.2"))) static int
simd_sse42_utf16le_to_utf8(const uint16_t *restrict src, size_t units,
                           uint8_t *restrict dst, size_t *out_len) {
  const __m128i m7f80 = _mm_set1_epi16((short)0xFF80);
  size_t i = 0, o = 0;
  while (i < units) {
    if (i + 16 <= units) {
      __m128i v0 = _mm_loadu_si128((const __m128i *)(src + i));
      __m128i v1 = _mm_loadu_si128((const __m128i *)(src + i + 8));
      if (_mm_testz_si128(_mm_or_si128(v0, v1), m7f80)) { /* all < 0x80 */
        _mm_storeu_si128((__m128i *)(dst + o), _mm_packus_epi16(v0, v1));
        i += 16; o += 16;
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

/* Fast path: an 8-unit block with no surrogate at all is well-formed; skip it. */
__attribute__((target("sse4.2"))) static int
simd_sse42_validate_utf16le(const uint16_t *restrict src, size_t units) {
  const __m128i f800 = _mm_set1_epi16((short)0xF800);
  const __m128i d800 = _mm_set1_epi16((short)0xD800);
  size_t i = 0;
  while (i < units) {
    if (i + 8 <= units) {
      __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
      __m128i issur = _mm_cmpeq_epi16(_mm_and_si128(v, f800), d800);
      if (_mm_movemask_epi8(issur) == 0) { i += 8; continue; } /* all BMP */
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

/* Count code points: per block, 8 minus the number of low-surrogate units.
 * Each 16-bit cmpeq lane sets 2 movemask bits, so popcount is halved. */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_count_utf16(const uint16_t *restrict src, size_t units) {
  const __m128i fc00 = _mm_set1_epi16((short)0xFC00);
  const __m128i dc00 = _mm_set1_epi16((short)0xDC00);
  size_t i = 0, total = 0;
  for (; i + 8 <= units; i += 8) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
    __m128i islow = _mm_cmpeq_epi16(_mm_and_si128(v, fc00), dc00);
    total += 8 - (size_t)(__builtin_popcount((unsigned)_mm_movemask_epi8(islow)) / 2);
  }
  for (; i < units; i++)
    if (src[i] < 0xDC00 || src[i] > 0xDFFF) total++;
  return total;
}

/* ── Override table ─────────────────────────────────────────────── */
/* ── Double-precision (f64) kernels — __m128d, 2 lanes (SSE2, always
 *    present on x86-64). Reductions run only full 2-lane bodies then a scalar
 *    tail (no seed load past x[n)); max/min guard the seed load for n < 2. ── */
__attribute__((target("sse4.2"))) static double
simd_sse42_f64_sum(const double *restrict x, size_t n) {
  __m128d acc = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 2 <= n; i += 2)
    acc = _mm_add_pd(acc, _mm_loadu_pd(&x[i]));
  double result = _mm_cvtsd_f64(_mm_add_sd(acc, _mm_unpackhi_pd(acc, acc)));
  for (; i < n; i++)
    result += x[i];
  return result;
}

__attribute__((target("sse4.2"))) static double
simd_sse42_f64_dot(const double *restrict a, const double *restrict b,
                   size_t n) {
  __m128d acc = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 2 <= n; i += 2)
    acc = _mm_add_pd(acc, _mm_mul_pd(_mm_loadu_pd(&a[i]), _mm_loadu_pd(&b[i])));
  double result = _mm_cvtsd_f64(_mm_add_sd(acc, _mm_unpackhi_pd(acc, acc)));
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

__attribute__((target("sse4.2"))) static double
simd_sse42_f64_max(const double *restrict x, size_t n) {
  if (n == 0)
    return -DBL_MAX;
  double result;
  size_t i;
  if (n >= 2) {
    __m128d vmax = _mm_loadu_pd(x);
    for (i = 2; i + 2 <= n; i += 2)
      vmax = _mm_max_pd(vmax, _mm_loadu_pd(&x[i]));
    result = _mm_cvtsd_f64(_mm_max_sd(vmax, _mm_unpackhi_pd(vmax, vmax)));
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("sse4.2"))) static double
simd_sse42_f64_min(const double *restrict x, size_t n) {
  if (n == 0)
    return DBL_MAX;
  double result;
  size_t i;
  if (n >= 2) {
    __m128d vmin = _mm_loadu_pd(x);
    for (i = 2; i + 2 <= n; i += 2)
      vmin = _mm_min_pd(vmin, _mm_loadu_pd(&x[i]));
    result = _mm_cvtsd_f64(_mm_min_sd(vmin, _mm_unpackhi_pd(vmin, vmin)));
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_f64_scale(double *restrict out, const double *restrict x, double s,
                     size_t n) {
  __m128d vs = _mm_set1_pd(s);
  size_t i = 0;
  for (; i + 2 <= n; i += 2)
    _mm_storeu_pd(&out[i], _mm_mul_pd(_mm_loadu_pd(&x[i]), vs));
  for (; i < n; i++)
    out[i] = x[i] * s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_f64_axpy(double *restrict y, double a, const double *restrict x,
                    size_t n) {
  __m128d va = _mm_set1_pd(a);
  size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    /* non-fused mul-then-add: SSE2 has no FMA, matching scalar/JS bit-for-bit */
    __m128d p = _mm_mul_pd(va, _mm_loadu_pd(&x[i]));
    _mm_storeu_pd(&y[i], _mm_add_pd(_mm_loadu_pd(&y[i]), p));
  }
  for (; i < n; i++) {
    double p = a * x[i];
    y[i] = y[i] + p;
  }
}

/* ── Signed 32-bit integer (i32) kernels — __m128i, 4 lanes (SSE4.1/4.2).
 * i32_sum widens int32->int64 (exact); i32_dot converts to f64 (exact product);
 * reductions run full 4-lane bodies then a scalar tail, and min/max guard the
 * seed load for n < 4. Prefix scans use _mm_slli_si128 lane shifts with an
 * identity fill (0 for sum; INT_MIN/-inf patched via move_ss/blend_ps, which are
 * bit-pattern moves valid for int lanes too) + a scalar block carry. Every
 * intrinsic is SSE4.2 or below. ───────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static int64_t
simd_sse42_i32_sum(const int32_t *restrict x, size_t n) {
  __m128i acc = _mm_setzero_si128(); /* 2 x int64 */
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i v = _mm_loadu_si128((const __m128i *)(x + i));
    acc = _mm_add_epi64(acc, _mm_cvtepi32_epi64(v)); /* low 2 int32 -> 2 int64 */
    acc = _mm_add_epi64(acc,
                        _mm_cvtepi32_epi64(_mm_unpackhi_epi64(v, v))); /* high 2 */
  }
  int64_t result =
      (int64_t)_mm_cvtsi128_si64(acc) + (int64_t)_mm_extract_epi64(acc, 1);
  for (; i < n; i++)
    result += (int64_t)x[i];
  return result;
}

__attribute__((target("sse4.2"))) static int simd_sse42_hmin_epi32(__m128i v) {
  v = _mm_min_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
  v = _mm_min_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
  return _mm_cvtsi128_si32(v);
}
__attribute__((target("sse4.2"))) static int simd_sse42_hmax_epi32(__m128i v) {
  v = _mm_max_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
  v = _mm_max_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
  return _mm_cvtsi128_si32(v);
}

__attribute__((target("sse4.2"))) static int32_t
simd_sse42_i32_min(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MAX;
  int32_t result;
  size_t i;
  if (n >= 4) {
    __m128i vmin = _mm_loadu_si128((const __m128i *)x);
    for (i = 4; i + 4 <= n; i += 4)
      vmin = _mm_min_epi32(vmin, _mm_loadu_si128((const __m128i *)(x + i)));
    result = simd_sse42_hmin_epi32(vmin);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("sse4.2"))) static int32_t
simd_sse42_i32_max(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MIN;
  int32_t result;
  size_t i;
  if (n >= 4) {
    __m128i vmax = _mm_loadu_si128((const __m128i *)x);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_epi32(vmax, _mm_loadu_si128((const __m128i *)(x + i)));
    result = simd_sse42_hmax_epi32(vmax);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("sse4.2"))) static double
simd_sse42_i32_dot(const int32_t *restrict a, const int32_t *restrict b,
                   size_t n) {
  __m128d acc = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i va = _mm_loadu_si128((const __m128i *)(a + i));
    __m128i vb = _mm_loadu_si128((const __m128i *)(b + i));
    acc = _mm_add_pd(acc, _mm_mul_pd(_mm_cvtepi32_pd(va),
                                     _mm_cvtepi32_pd(vb))); /* low 2 */
    acc = _mm_add_pd(acc,
                     _mm_mul_pd(_mm_cvtepi32_pd(_mm_unpackhi_epi64(va, va)),
                                _mm_cvtepi32_pd(_mm_unpackhi_epi64(vb, vb)))); /* hi 2 */
  }
  double result = _mm_cvtsd_f64(_mm_add_sd(acc, _mm_unpackhi_pd(acc, acc)));
  for (; i < n; i++)
    result += (double)a[i] * (double)b[i];
  return result;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_i32_add(int32_t *restrict out, const int32_t *restrict a,
                   const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_si128((__m128i *)(out + i),
                     _mm_add_epi32(_mm_loadu_si128((const __m128i *)(a + i)),
                                   _mm_loadu_si128((const __m128i *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_i32_mul(int32_t *restrict out, const int32_t *restrict a,
                   const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_si128((__m128i *)(out + i),
                     _mm_mullo_epi32(_mm_loadu_si128((const __m128i *)(a + i)),
                                     _mm_loadu_si128((const __m128i *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_i32_scale(int32_t *restrict out, const int32_t *restrict x, int32_t s,
                     size_t n) {
  __m128i vs = _mm_set1_epi32(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_si128((__m128i *)(out + i),
                     _mm_mullo_epi32(_mm_loadu_si128((const __m128i *)(x + i)),
                                     vs));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_f32_cumsum(float *out, const float *x, size_t n) {
  float carry = 0.0f;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 v = _mm_loadu_ps(x + i);
    v = _mm_add_ps(v, _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v), 4)));
    v = _mm_add_ps(v, _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v), 8)));
    v = _mm_add_ps(v, _mm_set1_ps(carry));
    _mm_storeu_ps(out + i, v);
    carry = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
  }
  for (; i < n; i++) {
    carry += x[i];
    out[i] = carry;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_i32_cumsum(int32_t *out, const int32_t *x, size_t n) {
  int32_t carry = 0;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i v = _mm_loadu_si128((const __m128i *)(x + i));
    v = _mm_add_epi32(v, _mm_slli_si128(v, 4));
    v = _mm_add_epi32(v, _mm_slli_si128(v, 8));
    v = _mm_add_epi32(v, _mm_set1_epi32(carry));
    _mm_storeu_si128((__m128i *)(out + i), v);
    carry = _mm_extract_epi32(v, 3);
  }
  for (; i < n; i++) {
    carry = (int32_t)((uint32_t)carry + (uint32_t)x[i]);
    out[i] = carry;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_f32_cummax(float *out, const float *x, size_t n) {
  const __m128 ninf = _mm_set1_ps(-INFINITY);
  float carry = -INFINITY;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 v = _mm_loadu_ps(x + i);
    __m128 s1 = _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v), 4));
    s1 = _mm_move_ss(s1, ninf); /* lane0 fill 0 -> -inf */
    v = _mm_max_ps(v, s1);
    __m128 s2 = _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v), 8));
    s2 = _mm_blend_ps(s2, ninf, 0x3); /* lanes 0,1 fill 0 -> -inf */
    v = _mm_max_ps(v, s2);
    v = _mm_max_ps(v, _mm_set1_ps(carry));
    _mm_storeu_ps(out + i, v);
    carry = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)));
  }
  for (; i < n; i++) {
    if (x[i] > carry)
      carry = x[i];
    out[i] = carry;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_i32_cummax(int32_t *out, const int32_t *x, size_t n) {
  const __m128i imin = _mm_set1_epi32(INT32_MIN);
  int32_t carry = INT32_MIN;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128i v = _mm_loadu_si128((const __m128i *)(x + i));
    __m128i s1 = _mm_slli_si128(v, 4);
    s1 = _mm_castps_si128(
        _mm_move_ss(_mm_castsi128_ps(s1), _mm_castsi128_ps(imin))); /* lane0 */
    v = _mm_max_epi32(v, s1);
    __m128i s2 = _mm_slli_si128(v, 8);
    s2 = _mm_castps_si128(_mm_blend_ps(_mm_castsi128_ps(s2),
                                       _mm_castsi128_ps(imin), 0x3)); /* lanes 0,1 */
    v = _mm_max_epi32(v, s2);
    v = _mm_max_epi32(v, _mm_set1_epi32(carry));
    _mm_storeu_si128((__m128i *)(out + i), v);
    carry = _mm_extract_epi32(v, 3);
  }
  for (; i < n; i++) {
    if (x[i] > carry)
      carry = x[i];
    out[i] = carry;
  }
}

/* 256-bit membership, two pshufb per 16 bytes. See the NEON twin for the
 * derivation; SSE has no 8-bit shift, so (v >> 3) is a 16-bit shift masked
 * back down to the byte. */
__attribute__((target("sse4.2")))
static size_t simd_sse42_find_bitmap(const uint8_t *restrict p, size_t n,
                                     const uint8_t *restrict bitmap) {
  const __m128i lo_tbl = _mm_loadu_si128((const __m128i *)bitmap);
  const __m128i hi_tbl = _mm_loadu_si128((const __m128i *)(bitmap + 16));
  const __m128i pow2 = _mm_setr_epi8(1,2,4,8,16,32,64,(char)128,0,0,0,0,0,0,0,0);
  const __m128i m0f = _mm_set1_epi8(0x0f);
  const __m128i m07 = _mm_set1_epi8(0x07);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i idx = _mm_and_si128(_mm_srli_epi16(v, 3), m0f);
    __m128i ml = _mm_shuffle_epi8(lo_tbl, idx);
    __m128i mh = _mm_shuffle_epi8(hi_tbl, idx);
    /* blendv selects on the SIGN bit, which is exactly v >= 0x80 */
    __m128i sel = _mm_blendv_epi8(ml, mh, v);
    __m128i bit = _mm_shuffle_epi8(pow2, _mm_and_si128(v, m07));
    __m128i hit = _mm_and_si128(sel, bit);
    /* member iff the AND is non-zero */
    int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(hit, _mm_setzero_si128()));
    mask = (~mask) & 0xffff;
    if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
  }
  for (; i < n; i++)
    if (bitmap[p[i] >> 3] & (uint8_t)(1u << (p[i] & 7))) return i;
  return SIZE_MAX;
}



/* base64 encode, 12 bytes -> 16 chars per iteration (Muła).
 *
 * x86 has no vld3q and pshufb indexes only 16 entries, so neither the NEON
 * de-interleave nor its 64-entry table applies. Instead: shuffle each 3-byte
 * group into 4 bytes, pull the four 6-bit fields out with two multiplies
 * (mulhi/mullo act as per-lane variable shifts), then map 0..63 to ASCII by
 * ADDING a per-range offset chosen through a 16-entry pshufb -- the ranges are
 * A-Z, a-z, 0-9, '+', '/', which is five offsets, not sixty-four entries. */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_base64_encode(const uint8_t *restrict src, size_t n,
                         char *restrict dst) {
  static const char E[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const __m128i shuf =
      _mm_set_epi8(10, 11, 9, 10, 7, 8, 6, 7, 4, 5, 3, 4, 1, 2, 0, 1);
  const __m128i shift_lut =
      _mm_setr_epi8('a' - 26, '0' - 52, '0' - 52, '0' - 52, '0' - 52,
                    '0' - 52, '0' - 52, '0' - 52, '0' - 52, '0' - 52,
                    '0' - 52, '+' - 62, '/' - 63, 'A', 0, 0);
  size_t i = 0, o = 0;
  /* 16-byte loads, so stop 4 bytes early to stay in bounds of the last group. */
  for (; i + 16 <= n; i += 12) {
    __m128i in = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(src + i)),
                                  shuf);
    __m128i t0 = _mm_and_si128(in, _mm_set1_epi32(0x0fc0fc00));
    __m128i t1 = _mm_mulhi_epu16(t0, _mm_set1_epi32(0x04000040));
    __m128i t2 = _mm_and_si128(in, _mm_set1_epi32(0x003f03f0));
    __m128i t3 = _mm_mullo_epi16(t2, _mm_set1_epi32(0x01000010));
    __m128i idx = _mm_or_si128(t1, t3);
    __m128i r = _mm_subs_epu8(idx, _mm_set1_epi8(51));
    __m128i lt = _mm_cmpgt_epi8(_mm_set1_epi8(26), idx);
    r = _mm_or_si128(r, _mm_and_si128(lt, _mm_set1_epi8(13)));
    r = _mm_add_epi8(_mm_shuffle_epi8(shift_lut, r), idx);
    _mm_storeu_si128((__m128i *)(dst + o), r);
    o += 16;
  }
  for (; i + 3 <= n; i += 3) {
    uint32_t x = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
    dst[o++] = E[(x >> 18) & 0x3F]; dst[o++] = E[(x >> 12) & 0x3F];
    dst[o++] = E[(x >> 6) & 0x3F];  dst[o++] = E[x & 0x3F];
  }
  if (n - i == 1) {
    uint32_t x = (uint32_t)src[i] << 16;
    dst[o++] = E[(x >> 18) & 0x3F]; dst[o++] = E[(x >> 12) & 0x3F];
    dst[o++] = '='; dst[o++] = '=';
  } else if (n - i == 2) {
    uint32_t x = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
    dst[o++] = E[(x >> 18) & 0x3F]; dst[o++] = E[(x >> 12) & 0x3F];
    dst[o++] = E[(x >> 6) & 0x3F];  dst[o++] = '=';
  }
  return o;
}

/* ---- the six slots that existed on NEON but not here ------------------
 * A portfolio hole is worse than no portfolio: the selection logic hides it,
 * the tests pass on the developer's machine, and one architecture quietly runs
 * the scalar path forever. These mirror the NEON kernels exactly. */

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_l2(const float *restrict a, const float *restrict b, size_t d) {
  return sqrtf(simd_sse42_dist_l2_sq(a, b, d));
}

/* The vector step is a BYPASS: it answers "is the value anywhere in these
   lanes", and only then does a scalar walk pin down which one. That keeps the
   common no-match case branch-free per block. */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_find_f32(const float *restrict p, float v, size_t n) {
  size_t i = 0;
  __m128 vv = _mm_set1_ps(v);
  for (; i + 4 <= n; i += 4)
    if (_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(p + i), vv)))
      for (size_t j = i; j < i + 4; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_find_f64(const double *restrict p, double v, size_t n) {
  size_t i = 0;
  __m128d vv = _mm_set1_pd(v);
  for (; i + 2 <= n; i += 2)
    if (_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(p + i), vv))) {
      if (p[i] == v) return i;
      if (p[i + 1] == v) return i + 1;
    }
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_find_u16(const uint16_t *restrict p, uint16_t v, size_t n) {
  size_t i = 0;
  __m128i vv = _mm_set1_epi16((short)v);
  for (; i + 8 <= n; i += 8)
    if (_mm_movemask_epi8(
            _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)(p + i)), vv)))
      for (size_t j = i; j < i + 8; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}

__attribute__((target("sse4.2"))) static size_t
simd_sse42_find_u32(const uint32_t *restrict p, uint32_t v, size_t n) {
  size_t i = 0;
  __m128i vv = _mm_set1_epi32((int)v);
  for (; i + 4 <= n; i += 4)
    if (_mm_movemask_epi8(
            _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(p + i)), vv)))
      for (size_t j = i; j < i + 4; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}

/* Schraudolph exponential, same constants as the NEON kernel. The magic is
   POSITIVE: a negative one double-negates and silently computes silu(-x). */
__attribute__((target("sse4.2"))) static void
simd_sse42_silu(float *restrict out, const float *restrict in, size_t n) {
  size_t i = 0;
  const __m128 magic = _mm_set1_ps(12102203.0f);
  const __m128 one = _mm_set1_ps(1.0f);
  const __m128 zerof = _mm_setzero_ps();
  const __m128i zeroi = _mm_setzero_si128();
  const __m128i inf = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= n; i += 4) {
    __m128 x = _mm_loadu_ps(&in[i]);
    __m128 neg_x = _mm_sub_ps(zerof, x);
    __m128i bits = _mm_cvttps_epi32(_mm_mul_ps(neg_x, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(one));
    bits = _mm_max_epi32(bits, zeroi);
    bits = _mm_min_epi32(bits, inf);
    __m128 exp_neg_x = _mm_max_ps(_mm_castsi128_ps(bits), zerof);
    __m128 sig = _mm_div_ps(one, _mm_add_ps(one, exp_neg_x));
    _mm_storeu_ps(&out[i], _mm_mul_ps(x, sig));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v / (1.0f + expf(-v));
  }
}

void simd_override_sse42(simd_t *t) {
  t->hex_encode = simd_sse42_hex_encode;
  t->hex_decode = simd_sse42_hex_decode;
  t->latin1_to_utf8 = simd_sse42_latin1_to_utf8;
  t->utf8_to_latin1 = simd_sse42_utf8_to_latin1;
  t->count_utf8 = simd_sse42_count_utf8;
  t->utf8_to_utf16le = simd_sse42_utf8_to_utf16le;
  t->utf16le_to_utf8 = simd_sse42_utf16le_to_utf8;
  t->validate_utf16le = simd_sse42_validate_utf16le;
  t->count_utf16 = simd_sse42_count_utf16;
  t->strfind = simd_sse42_strfind;
  t->count_u8 = simd_sse42_count_u8;
  t->find_first_of = simd_sse42_find_first_of;
  t->find_bitmap = simd_sse42_find_bitmap;
  t->validate_utf8 = simd_sse42_validate_utf8;
  t->dot = simd_sse42_dot;
  t->dot_f = simd_sse42_dot_f;
  t->norm_l2_sq = simd_sse42_norm_l2_sq;
  t->norm_l2 = simd_sse42_norm_l2;
  t->norm_l1 = simd_sse42_norm_l1;
  t->sum = simd_sse42_sum;
  t->max = simd_sse42_max;
  t->min = simd_sse42_min;
  t->argmax = simd_sse42_argmax;
  t->argmin = simd_sse42_argmin;
  t->argminmax = simd_sse42_argminmax;
  t->add = simd_sse42_add;
  t->sub = simd_sse42_sub;
  t->mul = simd_sse42_mul;
  t->div = simd_sse42_div;
  t->abs = simd_sse42_abs;
  t->fma = simd_sse42_fma;
  t->add_s = simd_sse42_add_s;
  t->mul_s = simd_sse42_mul_s;
  t->scale_add_s = simd_sse42_scale_add_s;
  t->sigmoid = simd_sse42_sigmoid;
  t->relu = simd_sse42_relu;
  t->relu6 = simd_sse42_relu6;
  t->leaky_relu = simd_sse42_leaky_relu;
  t->elu = simd_sse42_elu;
  t->tanh_fast = simd_sse42_tanh;
  t->gelu = simd_sse42_gelu;
  t->softmax = simd_sse42_softmax;
  t->log_softmax = simd_sse42_log_softmax;
  t->vexp = simd_sse42_vexp;
  t->vlog = simd_sse42_vlog;
  t->vsqrt = simd_sse42_vsqrt;
  t->vrsqrt = simd_sse42_vrsqrt;
  t->vinv = simd_sse42_vinv;
  t->dist_l2_sq = simd_sse42_dist_l2_sq;
  t->dist_l2 = simd_sse42_dist_l2;
  t->base64_encode = simd_sse42_base64_encode;
  t->find_f32 = simd_sse42_find_f32;
  t->find_f64 = simd_sse42_find_f64;
  t->find_u16 = simd_sse42_find_u16;
  t->find_u32 = simd_sse42_find_u32;
  t->silu = simd_sse42_silu;
  t->dist_l1 = simd_sse42_dist_l1;
  t->dist_cos = simd_sse42_dist_cos;
  t->dist_cheb = simd_sse42_dist_cheb;
  t->dist_matrix_l2_sq = simd_sse42_dist_matrix_l2_sq;
  t->dist_matrix_cos = simd_sse42_dist_matrix_cos;
  t->dist_matrix_l1 = simd_sse42_dist_matrix_l1;
  t->gemv = simd_sse42_gemv;
  t->gemv_t = simd_sse42_gemv_t;
  t->gemm = simd_sse42_gemm;
  t->threshold = simd_sse42_threshold;
  t->threshold_sign = simd_sse42_threshold_sign;
  t->hamming = simd_sse42_hamming;
  t->topk_indices = simd_sse42_topk_indices;
  t->clamp = simd_sse42_clamp;
  t->f64_sum = simd_sse42_f64_sum;
  t->f64_dot = simd_sse42_f64_dot;
  t->f64_min = simd_sse42_f64_min;
  t->f64_max = simd_sse42_f64_max;
  t->f64_scale = simd_sse42_f64_scale;
  t->f64_axpy = simd_sse42_f64_axpy;
  t->i32_sum = simd_sse42_i32_sum;
  t->i32_min = simd_sse42_i32_min;
  t->i32_max = simd_sse42_i32_max;
  t->i32_dot = simd_sse42_i32_dot;
  t->i32_add = simd_sse42_i32_add;
  t->i32_mul = simd_sse42_i32_mul;
  t->i32_scale = simd_sse42_i32_scale;
  t->f32_cumsum = simd_sse42_f32_cumsum;
  t->i32_cumsum = simd_sse42_i32_cumsum;
  t->f32_cummax = simd_sse42_f32_cummax;
  t->i32_cummax = simd_sse42_i32_cummax;
}

#else /* !x86_64 */
void simd_override_sse42(simd_t *t) { (void)t; }
#endif /* x86_64 */