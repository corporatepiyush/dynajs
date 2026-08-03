#include "dyna-simd-kernels.h"
/*
 * Copyright 2026 Piyush Katariya
 *
 * Licensed under the Apache License, Version 2.0; see LICENSE.
 */

/* AVX-512F + AVX-512BW + AVX-512DQ SIMD overrides — 512-bit ZMM registers.
 * The ISA check requires ALL THREE sub-features (see dispatch core).
 * 16 floats per vector where applicable. */

/* Whole-file guard: pairs with the #else/#endif stub at the bottom. A
 * stray early #endif here used to orphan that #else, so this file never
 * compiled on any host. */
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <math.h>
#include "dyna-simd-kernels.h"

/* Scalar fallback (simd_scalar.c) installed for ops that have
 * no AVX-512 kernel; not declared in the shared header. */
float simd_scalar_dist_l2(const float *restrict a,
                                 const float *restrict b, size_t d);
#include <math.h>

__attribute__((target("avx512f,avx512bw,avx512dq"))) static inline float
hsum512_ps(__m512 v) {
  __m256 lo = _mm512_castps512_ps256(v);
  __m256 hi = _mm512_extractf32x8_ps(v, 1);
  __m256 s = _mm256_add_ps(lo, hi);
  __m128 slo = _mm256_castps256_ps128(s);
  __m128 shi = _mm256_extractf128_ps(s, 1);
  slo = _mm_add_ps(slo, shi);
  slo = _mm_add_ps(slo, _mm_movehl_ps(slo, slo));
  slo = _mm_add_ss(slo, _mm_shuffle_ps(slo, slo, 1));
  return _mm_cvtss_f32(slo);
}

/* ── Dot product ──────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dot(const float *restrict a, const float *restrict b,
                       size_t n) {
  __m512 acc = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    acc = _mm512_fmadd_ps(va, vb, acc);
  }
  float result = hsum512_ps(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dot_f(const float *restrict a,
                         const float *restrict b, size_t n) {
  __m512d acc0 = _mm512_setzero_pd();
  __m512d acc1 = _mm512_setzero_pd();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    __m512 prod = _mm512_mul_ps(va, vb);
    acc0 = _mm512_add_pd(acc0, _mm512_cvtps_pd(_mm512_castps512_ps256(prod)));
    acc1 =
        _mm512_add_pd(acc1, _mm512_cvtps_pd(_mm512_extractf32x8_ps(prod, 1)));
  }
  double result = acc0[0] + acc0[1] + acc0[2] + acc0[3] + acc0[4] + acc0[5] +
                  acc0[6] + acc0[7] + acc1[0] + acc1[1] + acc1[2] + acc1[3] +
                  acc1[4] + acc1[5] + acc1[6] + acc1[7];
  for (; i < n; i++)
    result += (double)a[i] * b[i];
  return (float)result;
}

/* ── Norms ────────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_norm_l2_sq(const float *restrict x, size_t n) {
  return simd_avx512_dot(x, x, n);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_avx512_norm_l2_sq(x, n));
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_norm_l1(const float *restrict x, size_t n) {
  __m512 acc = _mm512_setzero_ps();
  __m512 sign_mask = (__m512)_mm512_set1_epi32(0x80000000);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vx = _mm512_loadu_ps(&x[i]);
    acc = _mm512_add_ps(acc, _mm512_andnot_ps(sign_mask, vx));
  }
  float result = hsum512_ps(acc);
  for (; i < n; i++)
    result += fabsf(x[i]);
  return result;
}

/* ── Sum ──────────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_sum(const float *restrict x, size_t n) {
  __m512 acc = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16)
    acc = _mm512_add_ps(acc, _mm512_loadu_ps(&x[i]));
  float result = hsum512_ps(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

/* ── Max / Min ────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_max(const float *restrict x, size_t n) {
  if (n == 0)
    return -FLT_MAX;
  if (n < 16) { /* 16-wide head load reads past x[n) for small n (OOB) */
    float m = x[0];
    for (size_t i = 1; i < n; i++) if (x[i] > m) m = x[i];
    return m;
  }
  __m512 vmax = _mm512_loadu_ps(x);
  size_t i = 16;
  for (; i + 16 <= n; i += 16)
    vmax = _mm512_max_ps(vmax, _mm512_loadu_ps(&x[i]));
  float result = _mm512_reduce_max_ps(vmax);
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_min(const float *restrict x, size_t n) {
  if (n == 0)
    return FLT_MAX;
  if (n < 16) { /* 16-wide head load reads past x[n) for small n (OOB) */
    float m = x[0];
    for (size_t i = 1; i < n; i++) if (x[i] < m) m = x[i];
    return m;
  }
  __m512 vmin = _mm512_loadu_ps(x);
  size_t i = 16;
  for (; i + 16 <= n; i += 16)
    vmin = _mm512_min_ps(vmin, _mm512_loadu_ps(&x[i]));
  float result = _mm512_reduce_min_ps(vmin);
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

/* ── Argmax / Argmin ──────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static size_t
simd_avx512_argmax(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  if (n < 16) { /* 16-wide head load reads past x[n) for small n (OOB) */
    size_t k = 0;
    for (size_t i = 1; i < n; i++) if (x[i] > x[k]) k = i;
    return k;
  }
  __m512 vmax = _mm512_loadu_ps(x);
  __m512i vidx =
      _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  __m512i vidx_max = vidx;
  size_t i = 16;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&x[i]);
    __m512i vidxi = _mm512_set1_epi32((int)i);
    __m512i step =
        _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    vidxi = _mm512_add_epi32(vidxi, step);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vmax, _CMP_GT_OS);
    vmax = _mm512_max_ps(vmax, vi);
    vidx_max = _mm512_mask_blend_epi32(mask, vidx_max, vidxi);
  }
  float tmp[16];
  uint32_t idx_tmp[16];
  _mm512_storeu_ps(tmp, vmax);
  _mm512_storeu_si512((__m512i *)idx_tmp, vidx_max);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 16; k++)
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

__attribute__((target("avx512f,avx512bw,avx512dq"))) static size_t
simd_avx512_argmin(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  if (n < 16) { /* 16-wide head load reads past x[n) for small n (OOB) */
    size_t k = 0;
    for (size_t i = 1; i < n; i++) if (x[i] < x[k]) k = i;
    return k;
  }
  __m512 vmin = _mm512_loadu_ps(x);
  __m512i vidx =
      _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  __m512i vidx_min = vidx;
  size_t i = 16;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&x[i]);
    __m512i vidxi = _mm512_set1_epi32((int)i);
    __m512i step =
        _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    vidxi = _mm512_add_epi32(vidxi, step);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vmin, _CMP_LT_OS);
    vmin = _mm512_min_ps(vmin, vi);
    vidx_min = _mm512_mask_blend_epi32(mask, vidx_min, vidxi);
  }
  float tmp[16];
  uint32_t idx_tmp[16];
  _mm512_storeu_ps(tmp, vmin);
  _mm512_storeu_si512((__m512i *)idx_tmp, vidx_min);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 16; k++)
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

/* ── Argminmax ────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_argminmax(const float *restrict x, size_t n,
                             size_t *argmin_out, size_t *argmax_out) {
  if (n == 0) {
    *argmin_out = *argmax_out = 0;
    return;
  }
  if (n < 16) { /* 16-wide head load reads past x[n) for small n (OOB) */
    size_t mn = 0, mx = 0;
    for (size_t i = 1; i < n; i++) { if (x[i] < x[mn]) mn = i; if (x[i] > x[mx]) mx = i; }
    *argmin_out = mn; *argmax_out = mx;
    return;
  }
  __m512 vmin = _mm512_loadu_ps(x);
  __m512 vmax = vmin;
  __m512i vidx =
      _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  __m512i vidx_min = vidx, vidx_max = vidx;
  size_t i = 16;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&x[i]);
    __m512i vidxi = _mm512_set1_epi32((int)i);
    __m512i step =
        _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    vidxi = _mm512_add_epi32(vidxi, step);
    __mmask16 mask_lt = _mm512_cmp_ps_mask(vi, vmin, _CMP_LT_OS);
    __mmask16 mask_gt = _mm512_cmp_ps_mask(vi, vmax, _CMP_GT_OS);
    vmin = _mm512_min_ps(vmin, vi);
    vmax = _mm512_max_ps(vmax, vi);
    vidx_min = _mm512_mask_blend_epi32(mask_lt, vidx_min, vidxi);
    vidx_max = _mm512_mask_blend_epi32(mask_gt, vidx_max, vidxi);
  }
  float tmp_min[16], tmp_max[16];
  uint32_t idx_min[16], idx_max[16];
  _mm512_storeu_ps(tmp_min, vmin);
  _mm512_storeu_ps(tmp_max, vmax);
  _mm512_storeu_si512((__m512i *)idx_min, vidx_min);
  _mm512_storeu_si512((__m512i *)idx_max, vidx_max);
  float best_min = tmp_min[0], best_max = tmp_max[0];
  size_t imin = idx_min[0], imax = idx_max[0];
  for (size_t k = 1; k < 16; k++) {
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
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_add(float *restrict z, const float *restrict a,
                       const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    _mm512_storeu_ps(&z[i], _mm512_add_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] + b[i];
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_sub(float *restrict z, const float *restrict a,
                       const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    _mm512_storeu_ps(&z[i], _mm512_sub_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] - b[i];
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_mul(float *restrict z, const float *restrict a,
                       const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    _mm512_storeu_ps(&z[i], _mm512_mul_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] * b[i];
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_div(float *restrict z, const float *restrict a,
                       const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    _mm512_storeu_ps(&z[i], _mm512_div_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] / b[i];
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_abs(float *restrict out, const float *restrict in,
                       size_t n) {
  __m512 sign_mask = (__m512)_mm512_set1_epi32(0x7FFFFFFF);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    _mm512_storeu_ps(&out[i], _mm512_and_ps(vi, sign_mask));
  }
  for (; i < n; i++)
    out[i] = fabsf(in[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_fma_krn(float *restrict z, const float *restrict a,
                           const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vz = _mm512_loadu_ps(&z[i]);
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    _mm512_storeu_ps(&z[i], _mm512_fmadd_ps(va, vb, vz));
  }
  for (; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ──────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_add_s(float *restrict z, const float *restrict x,
                         float s, size_t n) {
  __m512 vs = _mm512_set1_ps(s);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vx = _mm512_loadu_ps(&x[i]);
    _mm512_storeu_ps(&z[i], _mm512_add_ps(vx, vs));
  }
  for (; i < n; i++)
    z[i] = x[i] + s;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_mul_s(float *restrict z, const float *restrict x,
                         float s, size_t n) {
  __m512 vs = _mm512_set1_ps(s);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vx = _mm512_loadu_ps(&x[i]);
    _mm512_storeu_ps(&z[i], _mm512_mul_ps(vx, vs));
  }
  for (; i < n; i++)
    z[i] = x[i] * s;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_scale_add_s(float *restrict z, float alpha,
                               const float *restrict x, float beta,
                               size_t n) {
  __m512 va = _mm512_set1_ps(alpha);
  __m512 vb = _mm512_set1_ps(beta);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vx = _mm512_loadu_ps(&x[i]);
    _mm512_storeu_ps(&z[i], _mm512_fmadd_ps(va, vx, vb));
  }
  for (; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ──────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_sigmoid(float *restrict out,
                           const float *restrict in, size_t n) {
  __m512 one = _mm512_set1_ps(1.0f);
  __m512 lo = _mm512_set1_ps(-30.0f);
  __m512 hi = _mm512_set1_ps(30.0f);
  __m512 magic = _mm512_set1_ps(-12102203.0f);
  __m512 bias = _mm512_set1_ps(1.0f);
  __m512i zero = _mm512_setzero_si512();
  __m512i top = _mm512_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    vi = _mm512_max_ps(_mm512_min_ps(vi, hi), lo);
    __m512i bits = _mm512_cvtps_epi32(_mm512_mul_ps(vi, magic));
    bits = _mm512_add_epi32(bits, _mm512_castps_si512(bias));
    bits = _mm512_max_epi32(bits, zero);
    bits = _mm512_min_epi32(bits, top);
    __m512 ve = _mm512_add_ps(one, _mm512_castsi512_ps(bits));
    _mm512_storeu_ps(&out[i], _mm512_div_ps(one, ve));
  }
  for (; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_tanh_fast(float *restrict out,
                             const float *restrict in, size_t n) {
  __m512 two = _mm512_set1_ps(2.0f);
  __m512 one = _mm512_set1_ps(1.0f);
  __m512 lo = _mm512_set1_ps(-10.0f);
  __m512 hi = _mm512_set1_ps(10.0f);
  __m512 magic = _mm512_set1_ps(-12102203.0f * 2.0f);
  __m512 bias = _mm512_set1_ps(1.0f);
  __m512i zero = _mm512_setzero_si512();
  __m512i top = _mm512_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    vi = _mm512_max_ps(_mm512_min_ps(vi, hi), lo);
    __m512i bits = _mm512_cvtps_epi32(_mm512_mul_ps(vi, magic));
    bits = _mm512_add_epi32(bits, _mm512_castps_si512(bias));
    bits = _mm512_max_epi32(bits, zero);
    bits = _mm512_min_epi32(bits, top);
    __m512 ve = _mm512_sub_ps(
        _mm512_div_ps(two, _mm512_add_ps(one, _mm512_castsi512_ps(bits))), one);
    _mm512_storeu_ps(&out[i], ve);
  }
  for (; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_gelu(float *restrict out, const float *restrict in,
                        size_t n) {
  const __m512 sqrt_2_over_pi = _mm512_set1_ps(0.7978845608028654f);
  const __m512 c = _mm512_set1_ps(0.044715f);
  const __m512 half = _mm512_set1_ps(0.5f);
  const __m512 one = _mm512_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 x = _mm512_loadu_ps(&in[i]);
    __m512 x3 = _mm512_mul_ps(_mm512_mul_ps(x, x), x);
    __m512 inner = _mm512_mul_ps(sqrt_2_over_pi, _mm512_fmadd_ps(c, x3, x));
    /* _mm512_tanh_ps is SVML-only (ICC); per-lane libm tanh keeps the
     * lanes bit-identical to the scalar tail on GCC/Clang. */
    {
      float tmp[16];
      _mm512_storeu_ps(tmp, inner);
      for (int j = 0; j < 16; j++)
        tmp[j] = tanhf(tmp[j]);
      inner = _mm512_loadu_ps(tmp);
    }
    _mm512_storeu_ps(&out[i], _mm512_mul_ps(_mm512_mul_ps(half, x),
                                            _mm512_add_ps(one, inner)));
  }
  for (; i < n; i++) {
    float x = in[i];
    float x3 = x * x * x;
    float inner = tanhf(0.7978845608028654f * (x + 0.044715f * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_silu(float *restrict out, const float *restrict in,
                        size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 x = _mm512_loadu_ps(&in[i]);
    __m512 neg_x = _mm512_sub_ps(_mm512_setzero_ps(), x);
    __m512i bits =
        _mm512_cvtps_epi32(_mm512_mul_ps(neg_x, _mm512_set1_ps(-12102203.0f)));
    bits = _mm512_add_epi32(bits, _mm512_castps_si512(_mm512_set1_ps(1.0f)));
    bits = _mm512_max_epi32(bits, _mm512_setzero_si512());
    bits = _mm512_min_epi32(bits, _mm512_set1_epi32(0x7F800000));
    __m512 exp_neg_x =
        _mm512_max_ps(_mm512_castsi512_ps(bits), _mm512_setzero_ps());
    __m512 sigmoid_x = _mm512_div_ps(
        _mm512_set1_ps(1.0f), _mm512_add_ps(_mm512_set1_ps(1.0f), exp_neg_x));
    _mm512_storeu_ps(&out[i], _mm512_mul_ps(x, sigmoid_x));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v / (1.0f + expf(-v));
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_relu(float *restrict out, const float *restrict in,
                        size_t n) {
  __m512 zero = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    _mm512_storeu_ps(&out[i], _mm512_max_ps(vi, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_relu6(float *restrict out, const float *restrict in,
                         size_t n) {
  __m512 zero = _mm512_setzero_ps();
  __m512 six = _mm512_set1_ps(6.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    _mm512_storeu_ps(&out[i], _mm512_min_ps(_mm512_max_ps(vi, zero), six));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_leaky_relu(float *restrict out,
                              const float *restrict in, float slope,
                              size_t n) {
  __m512 vzero = _mm512_setzero_ps();
  __m512 vslope = _mm512_set1_ps(slope);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vzero, _CMP_GT_OS);
    _mm512_storeu_ps(&out[i],
                     _mm512_mask_mov_ps(_mm512_mul_ps(vi, vslope), mask, vi));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_elu(float *restrict out, const float *restrict in,
                       float alpha, size_t n) {
  __m512 vzero = _mm512_setzero_ps();
  __m512 valpha = _mm512_set1_ps(alpha);
  __m512 vone = _mm512_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vzero, _CMP_GT_OS);
    /* _mm512_exp_ps is SVML-only; per-lane libm exp matches the tail. */
    __m512 vexp;
    {
      float tmp[16];
      _mm512_storeu_ps(tmp, vi);
      for (int j = 0; j < 16; j++)
        tmp[j] = expf(tmp[j]);
      vexp = _mm512_loadu_ps(tmp);
    }
    __m512 exp_part = _mm512_sub_ps(vexp, vone);
    __m512 res_pos = vi;
    __m512 res_neg = _mm512_mul_ps(valpha, exp_part);
    _mm512_storeu_ps(&out[i], _mm512_mask_mov_ps(res_neg, mask, res_pos));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

/* Vectorized Schraudolph exp, matching fast_exp bit-for-bit:
 * bits = (int)(12102203·x + 0x3F800000), clamped to [0, +inf bits]. */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static inline __m512
avx512_fast_exp_ps(__m512 x) {
  __m512i bits = _mm512_cvtps_epi32(_mm512_fmadd_ps(
      x, _mm512_set1_ps(12102203.0f), _mm512_set1_ps(1065353216.0f)));
  bits = _mm512_max_epi32(bits, _mm512_setzero_si512());
  bits = _mm512_min_epi32(bits, _mm512_set1_epi32(0x7F800000));
  return _mm512_castsi512_ps(bits);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_softmax(float *restrict out,
                           const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  /* Max reduction. The 16-wide head load is only safe when n >= 16 —
   * softmax is routinely called with tiny n (class counts). */
  float maxv = in[0];
  size_t i = 1;
  if (n >= 16) {
    __m512 vmax = _mm512_loadu_ps(in);
    for (i = 16; i + 16 <= n; i += 16)
      vmax = _mm512_max_ps(vmax, _mm512_loadu_ps(&in[i]));
    maxv = _mm512_reduce_max_ps(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  __m512 vmaxv = _mm512_set1_ps(maxv);
  i = 0;
  for (; i + 16 <= n; i += 16)
    _mm512_storeu_ps(&out[i], avx512_fast_exp_ps(
                                  _mm512_sub_ps(_mm512_loadu_ps(&in[i]), vmaxv)));
  for (; i < n; i++)
    out[i] = fast_exp(in[i] - maxv);

  float inv_sum = 1.0f / simd_hsum_f32(out, n);
  __m512 vinv = _mm512_set1_ps(inv_sum);
  for (i = 0; i + 16 <= n; i += 16)
    _mm512_storeu_ps(&out[i], _mm512_mul_ps(_mm512_loadu_ps(&out[i]), vinv));
  for (; i < n; i++)
    out[i] *= inv_sum;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_log_softmax(float *restrict out,
                               const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  float maxv = in[0];
  size_t i = 1;
  if (n >= 16) {
    __m512 vmax = _mm512_loadu_ps(in);
    for (i = 16; i + 16 <= n; i += 16)
      vmax = _mm512_max_ps(vmax, _mm512_loadu_ps(&in[i]));
    maxv = _mm512_reduce_max_ps(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* Accumulate the exp-sum in a register. Storing exps into out[] would
   * clobber in[] when out aliases in (the binding calls log_softmax(a, a, n)),
   * and the final in[i]-voff loop still needs the original input — the old
   * "exps land in out[]" code produced a ~550% error on in-place input. */
  __m512 vmaxv = _mm512_set1_ps(maxv);
  __m512 vsum = _mm512_setzero_ps();
  i = 0;
  for (; i + 16 <= n; i += 16)
    vsum = _mm512_add_ps(
        vsum, avx512_fast_exp_ps(_mm512_sub_ps(_mm512_loadu_ps(&in[i]), vmaxv)));
  float sum = _mm512_reduce_add_ps(vsum);
  for (; i < n; i++)
    sum += fast_exp(in[i] - maxv);

  float log_s = logf(sum);
  __m512 voff = _mm512_set1_ps(maxv + log_s);
  for (i = 0; i + 16 <= n; i += 16)
    _mm512_storeu_ps(&out[i], _mm512_sub_ps(_mm512_loadu_ps(&in[i]), voff));
  for (; i < n; i++)
    out[i] = in[i] - maxv - log_s;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_vexp(float *restrict out, const float *restrict in,
                        size_t n) {
  __m512 magic = _mm512_set1_ps(12102203.0f);
  __m512 bias = _mm512_set1_ps(1.0f);
  __m512 vzero = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __m512i bits = _mm512_cvtps_epi32(_mm512_mul_ps(vi, magic));
    bits = _mm512_add_epi32(bits, _mm512_castps_si512(bias));
    bits = _mm512_max_epi32(bits, _mm512_setzero_si512());
    bits = _mm512_min_epi32(bits, _mm512_set1_epi32(0x7F800000));
    __m512 ve = _mm512_castsi512_ps(bits);
    _mm512_storeu_ps(&out[i], _mm512_max_ps(ve, vzero));
  }
  for (; i < n; i++)
    out[i] = fast_exp(in[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_vlog(float *restrict out, const float *restrict in,
                        size_t n) {
  __m512 vzero = _mm512_setzero_ps();
  __m512 vneg = _mm512_set1_ps(-FLT_MAX);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vzero, _CMP_GT_OS);
    /* _mm512_log_ps is SVML-only; per-lane libm log matches the tail. */
    __m512 vln;
    {
      float tmp[16];
      _mm512_storeu_ps(tmp, vi);
      for (int j = 0; j < 16; j++)
        tmp[j] = logf(tmp[j]);
      vln = _mm512_loadu_ps(tmp);
    }
    vln = _mm512_mask_mov_ps(vneg, mask, vln);
    _mm512_storeu_ps(&out[i], vln);
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_vsqrt(float *restrict out, const float *restrict in,
                         size_t n) {
  __m512 vzero = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vzero, _CMP_GE_OS);
    __m512 vs = _mm512_sqrt_ps(vi);
    vs = _mm512_mask_mov_ps(vzero, mask, vs);
    _mm512_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_vrsqrt(float *restrict out, const float *restrict in,
                          size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    _mm512_storeu_ps(&out[i], _mm512_rsqrt14_ps(vi));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_vinv(float *restrict out, const float *restrict in,
                        size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    _mm512_storeu_ps(&out[i], _mm512_rcp14_ps(vi));
  }
  for (; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dist_l2_sq(const float *restrict a,
                              const float *restrict b, size_t d) {
  __m512 acc = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= d; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    __m512 diff = _mm512_sub_ps(va, vb);
    acc = _mm512_fmadd_ps(diff, diff, acc);
  }
  float result = hsum512_ps(acc);
  for (; i < d; i++) {
    float df = a[i] - b[i];
    result += df * df;
  }
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dist_l1(const float *restrict a,
                           const float *restrict b, size_t d) {
  __m512 acc = _mm512_setzero_ps();
  __m512 sign_mask = (__m512)_mm512_set1_epi32(0x80000000);
  size_t i = 0;
  for (; i + 16 <= d; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    __m512 diff = _mm512_sub_ps(va, vb);
    acc = _mm512_add_ps(acc, _mm512_andnot_ps(sign_mask, diff));
  }
  float result = hsum512_ps(acc);
  for (; i < d; i++)
    result += fabsf(a[i] - b[i]);
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dist_cos(const float *restrict a,
                            const float *restrict b, size_t d) {
  __m512 vdot = _mm512_setzero_ps();
  __m512 vna = _mm512_setzero_ps();
  __m512 vnb = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= d; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    vdot = _mm512_fmadd_ps(va, vb, vdot);
    vna = _mm512_fmadd_ps(va, va, vna);
    vnb = _mm512_fmadd_ps(vb, vb, vnb);
  }
  float dot = hsum512_ps(vdot);
  float na = hsum512_ps(vna);
  float nb = hsum512_ps(vnb);
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

__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_dist_cheb(const float *restrict a,
                             const float *restrict b, size_t d) {
  __m512 vmax = _mm512_setzero_ps();
  __m512 sign_mask = (__m512)_mm512_set1_epi32(0x80000000);
  size_t i = 0;
  for (; i + 16 <= d; i += 16) {
    __m512 va = _mm512_loadu_ps(&a[i]);
    __m512 vb = _mm512_loadu_ps(&b[i]);
    __m512 adiff = _mm512_andnot_ps(sign_mask, _mm512_sub_ps(va, vb));
    vmax = _mm512_max_ps(vmax, adiff);
  }
  float result = _mm512_reduce_max_ps(vmax);
  for (; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > result)
      result = df;
  }
  return result;
}

/* ── Distance matrix ──────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_dist_matrix_l2_sq(float *restrict out,
                                     const float *restrict a,
                                     const float *restrict b, size_t n,
                                     size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx512_dist_l2_sq(&a[i * d], &b[j * d], d);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_dist_matrix_cos(float *restrict out,
                                   const float *restrict a,
                                   const float *restrict b, size_t n,
                                   size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx512_dist_cos(&a[i * d], &b[j * d], d);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_dist_matrix_l1(float *restrict out,
                                  const float *restrict a,
                                  const float *restrict b, size_t n,
                                  size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_avx512_dist_l1(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: gemv ───────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_gemv(float *restrict y, const float *restrict a,
                        const float *restrict x, size_t m, size_t n,
                        float beta) {
  for (size_t i = 0; i < m; i++) {
    __m512 acc0 = _mm512_setzero_ps();
    __m512 acc1 = _mm512_setzero_ps();
    size_t j = 0;
    for (; j + 32 <= n; j += 32) {
      acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(&a[i * n + j]),
                             _mm512_loadu_ps(&x[j]), acc0);
      acc1 = _mm512_fmadd_ps(_mm512_loadu_ps(&a[i * n + j + 16]),
                             _mm512_loadu_ps(&x[j + 16]), acc1);
    }
    for (; j + 16 <= n; j += 16)
      acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(&a[i * n + j]),
                             _mm512_loadu_ps(&x[j]), acc0);
    acc0 = _mm512_add_ps(acc0, acc1);
    float result = hsum512_ps(acc0);
    for (; j < n; j++)
      result += a[i * n + j] * x[j];
    y[i] = beta * y[i] + result;
  }
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_gemv_t(float *restrict y, const float *restrict a,
                          const float *restrict x, size_t m, size_t n,
                          float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    __m512 vxi = _mm512_set1_ps(x[i]);
    const float *row = &a[i * n];
    size_t j = 0;
    for (; j + 16 <= n; j += 16) {
      __m512 vy = _mm512_loadu_ps(&y[j]);
      __m512 va = _mm512_loadu_ps(&row[j]);
      _mm512_storeu_ps(&y[j], _mm512_fmadd_ps(vxi, va, vy));
    }
    for (; j < n; j++)
      y[j] += x[i] * row[j];
  }
}

/* ── BLAS Level 3: gemm (cache-tiled) ─────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_gemm(float *restrict c, const float *restrict a,
                        const float *restrict b, size_t m, size_t n,
                        size_t k, float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j + 16 <= n; j += 16) {
      __m512 vc = _mm512_loadu_ps(&c[i * n + j]);
      vc = _mm512_mul_ps(vc, _mm512_set1_ps(beta));
      _mm512_storeu_ps(&c[i * n + j], vc);
    }
  for (size_t j = n - (n % 16); j < n; j++)
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
            __m512 vaik = _mm512_set1_ps(alpha * a[i * k + kk]);
            size_t j = j0;
            for (; j + 16 <= jmax; j += 16) {
              __m512 vb = _mm512_loadu_ps(&b[kk * n + j]);
              __m512 vc = _mm512_loadu_ps(&c[i * n + j]);
              _mm512_storeu_ps(&c[i * n + j], _mm512_fmadd_ps(vaik, vb, vc));
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
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_threshold(float *restrict out,
                             const float *restrict in, float t, size_t n) {
  __m512 vt = _mm512_set1_ps(t);
  __m512 one = _mm512_set1_ps(1.0f);
  __m512 zero = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vt, _CMP_GT_OS);
    _mm512_storeu_ps(&out[i], _mm512_mask_blend_ps(mask, zero, one));
  }
  for (; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_threshold_sign(float *restrict out,
                                  const float *restrict in, float t,
                                  size_t n) {
  __m512 vt = _mm512_set1_ps(t);
  __m512 pos = _mm512_set1_ps(1.0f);
  __m512 neg = _mm512_set1_ps(-1.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __mmask16 mask = _mm512_cmp_ps_mask(vi, vt, _CMP_GE_OS);
    _mm512_storeu_ps(&out[i], _mm512_mask_blend_ps(mask, neg, pos));
  }
  for (; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ──────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static float
simd_avx512_hamming(const uint32_t *restrict a,
                           const uint32_t *restrict b, size_t n_words) {
  /* _mm512_popcnt_epi32 needs AVX512-VPOPCNTDQ, which is not in this
   * function's target set (and not checked by the dispatcher). Scalar
   * POPCNT over 64-bit words is the portable fast path here. */
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

/* ── Top-K (scalar heap) ──────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_topk_indices(const float *restrict vals,
                                uint32_t *restrict indices, size_t n,
                                size_t k) {
  simd_scalar_topk_indices(vals, indices, n, k);
}

/* ── Clamp ──────────────────────────────────────────────────────── */
__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_clamp(float *restrict out, const float *restrict in,
                         float lo, float hi, size_t n) {
  __m512 vlo = _mm512_set1_ps(lo);
  __m512 vhi = _mm512_set1_ps(hi);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512 vi = _mm512_loadu_ps(&in[i]);
    __m512 clamped = _mm512_min_ps(_mm512_max_ps(vi, vlo), vhi);
    _mm512_storeu_ps(&out[i], clamped);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* ── Override table ─────────────────────────────────────────────── */
/* ── Double-precision (f64) kernels — __m512d, 8 lanes (AVX-512). ────
 * Uses the intrinsic tree reductions (_mm512_reduce_{add,max,min}_pd).
 * Reductions run only full 8-lane bodies then a scalar tail; max/min take a
 * scalar path for n < 8 so the 8-wide seed load never reads past x[n).
 *
 * GATED OFF by default (DYNAJS_SIMD_F64_AVX512 undefined): this host's QEMU/
 * Rosetta amd64 emulation tops out at AVX2 and cannot execute AVX-512, so these
 * kernels are compile-clean but NOT runtime-proven under the differential
 * harness. Per the repo's "do not ship unproven x86 SIMD" rule they stay
 * disabled; AVX-512 hardware therefore runs the PROVEN AVX2 __m256d f64 kernels
 * (simd_init installs avx2 before avx512, and f64 is left un-overridden here).
 * Define DYNAJS_SIMD_F64_AVX512 once you can exercise them on AVX-512-capable
 * emulation/hardware via tests/test_simd_f64.c. */
#ifdef DYNAJS_SIMD_F64_AVX512
__attribute__((target("avx512f,avx512bw,avx512dq"))) static double
simd_avx512_f64_sum(const double *restrict x, size_t n) {
  __m512d acc = _mm512_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    acc = _mm512_add_pd(acc, _mm512_loadu_pd(&x[i]));
  double result = _mm512_reduce_add_pd(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static double
simd_avx512_f64_dot(const double *restrict a, const double *restrict b,
                    size_t n) {
  __m512d acc = _mm512_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    acc = _mm512_fmadd_pd(_mm512_loadu_pd(&a[i]), _mm512_loadu_pd(&b[i]), acc);
  double result = _mm512_reduce_add_pd(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static double
simd_avx512_f64_max(const double *restrict x, size_t n) {
  if (n == 0)
    return -DBL_MAX;
  if (n < 8) { /* 8-wide seed load would read past x[n) for small n (OOB) */
    double m = x[0];
    for (size_t i = 1; i < n; i++)
      if (x[i] > m)
        m = x[i];
    return m;
  }
  __m512d vmax = _mm512_loadu_pd(x);
  size_t i = 8;
  for (; i + 8 <= n; i += 8)
    vmax = _mm512_max_pd(vmax, _mm512_loadu_pd(&x[i]));
  double result = _mm512_reduce_max_pd(vmax);
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static double
simd_avx512_f64_min(const double *restrict x, size_t n) {
  if (n == 0)
    return DBL_MAX;
  if (n < 8) { /* 8-wide seed load would read past x[n) for small n (OOB) */
    double m = x[0];
    for (size_t i = 1; i < n; i++)
      if (x[i] < m)
        m = x[i];
    return m;
  }
  __m512d vmin = _mm512_loadu_pd(x);
  size_t i = 8;
  for (; i + 8 <= n; i += 8)
    vmin = _mm512_min_pd(vmin, _mm512_loadu_pd(&x[i]));
  double result = _mm512_reduce_min_pd(vmin);
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_f64_scale(double *restrict out, const double *restrict x, double s,
                      size_t n) {
  __m512d vs = _mm512_set1_pd(s);
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    _mm512_storeu_pd(&out[i], _mm512_mul_pd(_mm512_loadu_pd(&x[i]), vs));
  for (; i < n; i++)
    out[i] = x[i] * s;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_f64_axpy(double *restrict y, double a, const double *restrict x,
                     size_t n) {
  __m512d va = _mm512_set1_pd(a);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    /* non-fused mul-then-add (NOT fmadd): bit-exact vs scalar/JS y[i]+=a*x[i] */
    __m512d p = _mm512_mul_pd(va, _mm512_loadu_pd(&x[i]));
    _mm512_storeu_pd(&y[i], _mm512_add_pd(_mm512_loadu_pd(&y[i]), p));
  }
  for (; i < n; i++) {
    double p = a * x[i];
    y[i] = y[i] + p;
  }
}
#endif /* DYNAJS_SIMD_F64_AVX512 */

/* ── Signed 32-bit integer (i32) kernels — __m512i, 16 lanes (AVX-512).
 * GATED OFF by default (DYNAJS_SIMD_INT_AVX512 undefined): this host cannot
 * execute AVX-512, so these use the intrinsic tree reductions
 * (_mm512_reduce_{add,min,max}) and are compile-checked but NOT runtime-proven.
 * They stay disabled; AVX-512 hardware runs the PROVEN AVX2/SSE4.2 i32 kernels
 * via simd_init override ordering (sse42 runs first, avx2 next, and this
 * override leaves the slots untouched). The prefix scans (cumsum/cummax) have no
 * clean AVX-512 primitive, so they are intentionally NOT overridden here — they
 * fall back to the AVX2/SSE4.2 scan. Define DYNAJS_SIMD_INT_AVX512 to exercise
 * these on real AVX-512 via tests/test_simd_int.c. */
#ifdef DYNAJS_SIMD_INT_AVX512
__attribute__((target("avx512f,avx512bw,avx512dq"))) static int64_t
simd_avx512_i32_sum(const int32_t *restrict x, size_t n) {
  __m512i acc = _mm512_setzero_si512(); /* 8 x int64 */
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512i v = _mm512_loadu_si512((const void *)(x + i));
    acc = _mm512_add_epi64(acc,
                           _mm512_cvtepi32_epi64(_mm512_castsi512_si256(v)));
    acc = _mm512_add_epi64(
        acc, _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(v, 1)));
  }
  int64_t result = _mm512_reduce_add_epi64(acc);
  for (; i < n; i++)
    result += (int64_t)x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static int32_t
simd_avx512_i32_min(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MAX;
  if (n < 16) { /* the 16-wide seed load would read past x[n) for small n */
    int32_t m = x[0];
    for (size_t i = 1; i < n; i++)
      if (x[i] < m)
        m = x[i];
    return m;
  }
  __m512i vmin = _mm512_loadu_si512((const void *)x);
  size_t i = 16;
  for (; i + 16 <= n; i += 16)
    vmin = _mm512_min_epi32(vmin, _mm512_loadu_si512((const void *)(x + i)));
  int32_t result = _mm512_reduce_min_epi32(vmin);
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static int32_t
simd_avx512_i32_max(const int32_t *restrict x, size_t n) {
  if (n == 0)
    return INT32_MIN;
  if (n < 16) {
    int32_t m = x[0];
    for (size_t i = 1; i < n; i++)
      if (x[i] > m)
        m = x[i];
    return m;
  }
  __m512i vmax = _mm512_loadu_si512((const void *)x);
  size_t i = 16;
  for (; i + 16 <= n; i += 16)
    vmax = _mm512_max_epi32(vmax, _mm512_loadu_si512((const void *)(x + i)));
  int32_t result = _mm512_reduce_max_epi32(vmax);
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static double
simd_avx512_i32_dot(const int32_t *restrict a, const int32_t *restrict b,
                    size_t n) {
  __m512d acc = _mm512_setzero_pd();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m512i va = _mm512_loadu_si512((const void *)(a + i));
    __m512i vb = _mm512_loadu_si512((const void *)(b + i));
    acc = _mm512_fmadd_pd(_mm512_cvtepi32_pd(_mm512_castsi512_si256(va)),
                          _mm512_cvtepi32_pd(_mm512_castsi512_si256(vb)), acc);
    acc = _mm512_fmadd_pd(_mm512_cvtepi32_pd(_mm512_extracti64x4_epi64(va, 1)),
                          _mm512_cvtepi32_pd(_mm512_extracti64x4_epi64(vb, 1)),
                          acc);
  }
  double result = _mm512_reduce_add_pd(acc);
  for (; i < n; i++)
    result += (double)a[i] * (double)b[i];
  return result;
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_i32_add(int32_t *restrict out, const int32_t *restrict a,
                    const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16)
    _mm512_storeu_si512(
        (void *)(out + i),
        _mm512_add_epi32(_mm512_loadu_si512((const void *)(a + i)),
                         _mm512_loadu_si512((const void *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_i32_mul(int32_t *restrict out, const int32_t *restrict a,
                    const int32_t *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 16 <= n; i += 16)
    _mm512_storeu_si512(
        (void *)(out + i),
        _mm512_mullo_epi32(_mm512_loadu_si512((const void *)(a + i)),
                           _mm512_loadu_si512((const void *)(b + i))));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

__attribute__((target("avx512f,avx512bw,avx512dq"))) static void
simd_avx512_i32_scale(int32_t *restrict out, const int32_t *restrict x,
                      int32_t s, size_t n) {
  __m512i vs = _mm512_set1_epi32(s);
  size_t i = 0;
  for (; i + 16 <= n; i += 16)
    _mm512_storeu_si512(
        (void *)(out + i),
        _mm512_mullo_epi32(_mm512_loadu_si512((const void *)(x + i)), vs));
  for (; i < n; i++)
    out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}
#endif /* DYNAJS_SIMD_INT_AVX512 */

void simd_override_avx512(simd_t *t) {
  t->dot = simd_avx512_dot;
  t->dot_f = simd_avx512_dot_f;
  t->norm_l2_sq = simd_avx512_norm_l2_sq;
  t->norm_l2 = simd_avx512_norm_l2;
  t->norm_l1 = simd_avx512_norm_l1;
  t->sum = simd_avx512_sum;
  t->max = simd_avx512_max;
  t->min = simd_avx512_min;
  t->argmax = simd_avx512_argmax;
  t->argmin = simd_avx512_argmin;
  t->argminmax = simd_avx512_argminmax;
  t->add = simd_avx512_add;
  t->sub = simd_avx512_sub;
  t->mul = simd_avx512_mul;
  t->div = simd_avx512_div;
  t->abs = simd_avx512_abs;
  t->fma = simd_avx512_fma_krn;
  t->add_s = simd_avx512_add_s;
  t->mul_s = simd_avx512_mul_s;
  t->scale_add_s = simd_avx512_scale_add_s;
  t->sigmoid = simd_avx512_sigmoid;
  t->relu = simd_avx512_relu;
  t->relu6 = simd_avx512_relu6;
  t->leaky_relu = simd_avx512_leaky_relu;
  t->elu = simd_avx512_elu;
  t->tanh_fast = simd_avx512_tanh_fast;
  t->gelu = simd_avx512_gelu;
  t->silu = simd_avx512_silu;
  t->softmax = simd_avx512_softmax;
  t->log_softmax = simd_avx512_log_softmax;
  t->vexp = simd_avx512_vexp;
  t->vlog = simd_avx512_vlog;
  t->vsqrt = simd_avx512_vsqrt;
  t->vrsqrt = simd_avx512_vrsqrt;
  t->vinv = simd_avx512_vinv;
  t->dist_l2_sq = simd_avx512_dist_l2_sq;
  t->dist_l2 = simd_scalar_dist_l2; /* no AVX512 override, use scalar */
  t->dist_l1 = simd_avx512_dist_l1;
  t->dist_cos = simd_avx512_dist_cos;
  t->dist_cheb = simd_avx512_dist_cheb;
  t->dist_matrix_l2_sq = simd_avx512_dist_matrix_l2_sq;
  t->dist_matrix_cos = simd_avx512_dist_matrix_cos;
  t->dist_matrix_l1 = simd_avx512_dist_matrix_l1;
  t->gemv = simd_avx512_gemv;
  t->gemv_t = simd_avx512_gemv_t;
  t->gemm = simd_avx512_gemm;
  t->threshold = simd_avx512_threshold;
  t->threshold_sign = simd_avx512_threshold_sign;
  t->hamming = simd_avx512_hamming;
  t->topk_indices = simd_avx512_topk_indices;
  t->clamp = simd_avx512_clamp;
#ifdef DYNAJS_SIMD_F64_AVX512
  /* Off by default — see the gate comment above. AVX-512 HW keeps the proven
   * AVX2 f64 kernels installed by simd_override_avx2 (which runs first). */
  t->f64_sum = simd_avx512_f64_sum;
  t->f64_dot = simd_avx512_f64_dot;
  t->f64_min = simd_avx512_f64_min;
  t->f64_max = simd_avx512_f64_max;
  t->f64_scale = simd_avx512_f64_scale;
  t->f64_axpy = simd_avx512_f64_axpy;
#endif
#ifdef DYNAJS_SIMD_INT_AVX512
  /* Off by default — see the gate comment above. AVX-512 HW otherwise keeps the
   * PROVEN AVX2/SSE4.2 i32 kernels. Prefix scans are deliberately left to the
   * AVX2/SSE4.2 path (no clean AVX-512 prefix primitive). */
  t->i32_sum = simd_avx512_i32_sum;
  t->i32_min = simd_avx512_i32_min;
  t->i32_max = simd_avx512_i32_max;
  t->i32_dot = simd_avx512_i32_dot;
  t->i32_add = simd_avx512_i32_add;
  t->i32_mul = simd_avx512_i32_mul;
  t->i32_scale = simd_avx512_i32_scale;
#endif
}

#else /* !x86_64 */
void simd_override_avx512(simd_t *t) { (void)t; }
#endif /* x86_64 */