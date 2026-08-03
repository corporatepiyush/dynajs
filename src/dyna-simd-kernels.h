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

/* Portable SIMD dispatch layer for ML kernels.
 *
 * Architecture:
 *   - Dispatch table (simd_t) holds function pointers for every kernel.
 *   - At init time, cpu_features() detects the best ISA available and
 *     simd_init_impl() installs the matching override.
 *   - Priority (highest first): AVX-512F+BW+DQ > AVX2+FMA > SSE4.2 > SVE > NEON
 * > Scalar
 *
 * Every kernel has a scalar fallback so the table is always fully populated.
 * Per-ISA modules (simd_{avx2,avx512,neon,sve}.c) override the slots
 * they can accelerate; unaccelerated slots retain the scalar implementation.
 *
 * All "float" kernels operate on float arrays; "double" variants use double.
 * "n" is element count, "d" is dimensionality, alignment is 32 bytes.
 */

#ifndef DYNAJS_SIMD_KERNELS_H
#define DYNAJS_SIMD_KERNELS_H

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* x86 SIMD intrinsics (__m128/__m256/_mm_*): the per-ISA files and the core's
 * hsum helper need these; the original external build pulled them via a shared
 * ml header, which we drop here.
 * ARM NEON's arm_neon.h is included by the neon file under its own arch gate. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* ══════════════════════════════════════════════════════════════════
 * CPU feature flags
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
  CPU_SSE42 = 1u << 0,    /* x86 SSE4.2 */
  CPU_AVX2 = 1u << 1,     /* x86 AVX2 + FMA */
  CPU_AVX512F = 1u << 2,  /* x86 AVX-512 Foundation */
  CPU_AVX512BW = 1u << 3, /* x86 AVX-512 Byte/Word */
  CPU_AVX512DQ = 1u << 4, /* x86 AVX-512 DoubleWord/QuadWord */
  CPU_NEON = 1u << 5,     /* ARM64 NEON (ASIMD) */
  CPU_SVE = 1u << 6,      /* ARM64 SVE / SVE2 */
} cpu_feature_t;

/* Detect available CPU SIMD features at runtime.
 * Uses CPUID on x86-64 (leaf 1/7), getauxval(AT_HWCAP) on ARM64 Linux,
 * sysctl on macOS. Returns bitmask of cpu_feature_t. */
uint64_t cpu_features(void);

/* ══════════════════════════════════════════════════════════════════
 * Dispatch table — one entry per kernel
 * ══════════════════════════════════════════════════════════════════ */
typedef struct simd {
  /* ── BLAS Level 1: vector–vector ───────────────────────────── */
  float (*dot_f)(const float *restrict a, const float *restrict b,
                 size_t n);
  float (*dot)(const float *restrict a, const float *restrict b,
               size_t n);
  float (*norm_l2_sq)(const float *restrict x, size_t n);
  float (*norm_l2)(const float *restrict x, size_t n);
  float (*norm_l1)(const float *restrict x, size_t n);
  void (*axpy)(float *restrict y, float alpha, const float *restrict x,
               size_t n);
  void (*axpby)(float *restrict z, float a, const float *restrict x,
                float b, const float *restrict y, size_t n);

  /* ── Reductions ───────────────────────────────────────────── */
  float (*sum)(const float *restrict x, size_t n);
  float (*max)(const float *restrict x, size_t n);
  float (*min)(const float *restrict x, size_t n);
  size_t (*argmax)(const float *restrict x, size_t n);
  size_t (*argmin)(const float *restrict x, size_t n);
  void (*argminmax)(const float *restrict x, size_t n, size_t *argmin_out,
                    size_t *argmax_out);

  /* ── Element-wise: vector–vector → vector ────────────────── */
  void (*add)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*sub)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*mul)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*div)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*abs)(float *restrict out, const float *restrict in, size_t n);
  void (*fma)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);

  /* ── Element-wise: scalar–vector → vector ────────────────── */
  void (*add_s)(float *restrict z, const float *restrict x, float s,
                size_t n);
  void (*mul_s)(float *restrict z, const float *restrict x, float s,
                size_t n);
  void (*scale_add_s)(float *restrict z, float alpha,
                      const float *restrict x, float beta, size_t n);

  /* ── Activations (in-place safe: out may alias in) ───────── */
  void (*sigmoid)(float *restrict out, const float *restrict in,
                  size_t n);
  void (*relu)(float *restrict out, const float *restrict in, size_t n);
  void (*relu6)(float *restrict out, const float *restrict in,
                size_t n);
  void (*leaky_relu)(float *restrict out, const float *restrict in,
                     float slope, size_t n);
  void (*elu)(float *restrict out, const float *restrict in,
              float alpha, size_t n);
  void (*tanh_fast)(float *restrict out, const float *restrict in,
                    size_t n);
  void (*gelu)(float *restrict out, const float *restrict in, size_t n);
  /* BROKEN ON x86: the AVX2 and AVX-512 silu kernels compute silu(-x)
     (~3e5% error); only NEON and scalar are correct. dyna-simd.c's JS binding
     therefore composes sigmoid+mul instead of calling this, so the API is right
     on every ISA -- but a NEW C caller of this slot gets the wrong answer on
     x86. Do not use until the amd64 differential harness verifies it. */
  void (*silu)(float *restrict out, const float *restrict in, size_t n);

  /* ── Softmax family ──────────────────────────────────────── */
  void (*softmax)(float *restrict out, const float *restrict in,
                  size_t n);
  void (*log_softmax)(float *restrict out, const float *restrict in,
                      size_t n);

  /* ── Element-wise unary math ──────────────────────────────── */
  void (*vexp)(float *restrict out, const float *restrict in, size_t n);
  void (*vlog)(float *restrict out, const float *restrict in, size_t n);
  void (*vsqrt)(float *restrict out, const float *restrict in,
                size_t n);
  void (*vrsqrt)(float *restrict out, const float *restrict in,
                 size_t n);
  void (*vinv)(float *restrict out, const float *restrict in, size_t n);

  /* ── Distance: vector–vector → scalar ────────────────────── */
  float (*dist_l2_sq)(const float *restrict a, const float *restrict b,
                      size_t d);
  float (*dist_l1)(const float *restrict a, const float *restrict b,
                   size_t d);
  float (*dist_cos)(const float *restrict a, const float *restrict b,
                    size_t d);
  float (*dist_cheb)(const float *restrict a, const float *restrict b,
                     size_t d);
  float (*dist_l2)(const float *restrict a, const float *restrict b,
                   size_t d);

  /* ── Distance matrix: batch computation ──────────────────── */
  void (*dist_matrix_l2_sq)(float *restrict out,
                            const float *restrict a,
                            const float *restrict b, size_t n, size_t m,
                            size_t d);
  void (*dist_matrix_cos)(float *restrict out, const float *restrict a,
                          const float *restrict b, size_t n, size_t m,
                          size_t d);
  void (*dist_matrix_l1)(float *restrict out, const float *restrict a,
                         const float *restrict b, size_t n, size_t m,
                         size_t d);

  /* ── BLAS Level 2: matrix–vector ─────────────────────────── */
  void (*gemv)(float *restrict y, const float *restrict a,
               const float *restrict x, size_t m, size_t n, float beta);
  void (*gemv_t)(float *restrict y, const float *restrict a,
                 const float *restrict x, size_t m, size_t n, float beta);

  /* ── BLAS Level 3: matrix–matrix ─────────────────────────── */
  void (*gemm)(float *restrict c, const float *restrict a,
               const float *restrict b, size_t m, size_t n, size_t k,
               float alpha, float beta);

  /* ── Comparison / Selection ──────────────────────────────── */
  void (*threshold)(float *restrict out, const float *restrict in,
                    float t, size_t n);
  void (*threshold_sign)(float *restrict out, const float *restrict in,
                         float t, size_t n);

  /* ── Hamming ─────────────────────────────────────────────── */
  float (*hamming)(const uint32_t *restrict a,
                   const uint32_t *restrict b, size_t n_words);

  /* ── Scalar fast math (for inner loops) ──────────────────── */
  float (*sigmoid_f)(float x);
  float (*tanh_f)(float x);
  float (*exp_f)(float x);

  /* ── Top-K selection ─────────────────────────────────────── */
  void (*topk_indices)(const float *restrict vals,
                       uint32_t *restrict indices, size_t n, size_t k);

  /* ── Clamp ───────────────────────────────────────────────── */
  void (*clamp)(float *restrict out, const float *restrict in, float lo,
                float hi, size_t n);

  /* ── Double-precision (f64) array kernels ────────────────────────────
   * JS `Number` IS f64, so these run zero-copy over Float64Array. Reductions
   * (f64_sum/f64_dot) reorder additions vs a sequential scalar loop and are
   * therefore NOT bit-identical (compare with a relative tolerance). f64_min/
   * f64_max/f64_scale/f64_axpy ARE bit-exact vs a scalar reference: axpy uses a
   * NON-fused multiply-then-add (`y[i] += a*x[i]` in two rounding steps) so it
   * matches JS/scalar exactly on every ISA. Every reduction early-returns a
   * scalar loop for 0 < n < vector-width BEFORE any full-width load (no OOB). */
  double (*f64_sum)(const double *restrict x, size_t n);
  double (*f64_dot)(const double *restrict a, const double *restrict b,
                    size_t n);
  double (*f64_min)(const double *restrict x, size_t n);
  double (*f64_max)(const double *restrict x, size_t n);
  void (*f64_scale)(double *restrict out, const double *restrict x, double s,
                    size_t n);
  void (*f64_axpy)(double *restrict y, double a, const double *restrict x,
                   size_t n);

  /* ── Signed 32-bit integer (i32) array kernels ───────────────────────
   * Zero-copy over Int32Array. Wrapping ops match JS two's-complement:
   * i32_add wraps mod 2^32 like `(a+b)|0`; i32_mul/i32_scale keep the low 32
   * bits like Math.imul. Reductions are EXACT and bit-identical to a sequential
   * scalar loop on every ISA: i32_sum accumulates in int64 (widening, so it
   * cannot overflow for any realistic length and integer add is associative),
   * i32_min/i32_max are exact int32. i32_dot accumulates the (exact) int64
   * products in a double and returns a JS Number; like every float reduction it
   * reorders the additions, so it matches a sequential scalar dot only to a
   * relative tolerance (bit-exact while every partial result stays <= 2^53).
   * Every reduction early-returns a scalar loop for 0 < n < vector-width BEFORE
   * any full-width load (no heap-OOB read). */
  int64_t (*i32_sum)(const int32_t *restrict x, size_t n);
  int32_t (*i32_min)(const int32_t *restrict x, size_t n);
  int32_t (*i32_max)(const int32_t *restrict x, size_t n);
  double (*i32_dot)(const int32_t *restrict a, const int32_t *restrict b,
                    size_t n);
  void (*i32_add)(int32_t *restrict out, const int32_t *restrict a,
                  const int32_t *restrict b, size_t n);
  void (*i32_mul)(int32_t *restrict out, const int32_t *restrict a,
                  const int32_t *restrict b, size_t n);
  void (*i32_scale)(int32_t *restrict out, const int32_t *restrict x, int32_t s,
                    size_t n);

  /* ── Inclusive prefix scans — cumsum: out[i]=sum(x[0..i]); cummax:
   *    out[i]=max(x[0..i]). The pointers are deliberately NOT `restrict`: an
   *    in-place scan (out == x) must be well-defined. The i32 scans are EXACT
   *    (integer add wraps mod 2^32 and is associative; max is associative), so
   *    the SIMD result is bit-identical to the sequential scalar scan.
   *    f32_cumsum REORDERS the float additions vs a sequential left-fold, so it
   *    matches only to a relative tolerance; f32_cummax is exact (max rounds
   *    nothing). Scans run full vector-width blocks then a scalar tail — the
   *    block loop guard means no load ever reads past x[n). */
  void (*f32_cumsum)(float *out, const float *x, size_t n);
  void (*i32_cumsum)(int32_t *out, const int32_t *x, size_t n);
  void (*f32_cummax)(float *out, const float *x, size_t n);
  void (*i32_cummax)(int32_t *out, const int32_t *x, size_t n);

  /* ── Forward value search: first index of `v` in p[0..n), or SIZE_MAX.
   *    Used by String.indexOf/includes (find_u8/u16) and TypedArray
   *    indexOf/includes (find_u8/u16/u32/f32/f64). ─────────────────── */
  size_t (*find_u8)(const uint8_t *restrict p, uint8_t v, size_t n);
  size_t (*find_u16)(const uint16_t *restrict p, uint16_t v, size_t n);
  size_t (*find_u32)(const uint32_t *restrict p, uint32_t v, size_t n);
  size_t (*find_f32)(const float *restrict p, float v, size_t n);
  size_t (*find_f64)(const double *restrict p, double v, size_t n);

  /* ── SIMD substring search: first index of pat[0..m) in text[0..n), or
   *    SIZE_MAX. The "generic SIMD" first+last algorithm (Muła): vector-compare
   *    the pattern's first and last byte against the text, then memcmp the
   *    interior only at candidate positions. Empty pattern => 0. Used by
   *    dyna:strings and any delimiter/substring scan. ─────────────────────── */
  size_t (*strfind)(const uint8_t *text, size_t n, const uint8_t *pat,
                    size_t m);

  /* ── Byte-scanning / text kernels (CSV, JSON, log processing). ──────── */
  /* Count occurrences of byte `v` in p[0..n) (newline/row/field counting). */
  size_t (*count_u8)(const uint8_t *restrict p, uint8_t v, size_t n);
  /* First index in p[0..n) whose byte is any of set[0..setlen), or SIZE_MAX.
   * setlen<=8 is vectorised (OR of compares); larger sets fall back to a
   * 256-entry membership table. Delimiter/quote/newline/structural scan. */
  size_t (*find_first_of)(const uint8_t *restrict p, size_t n,
                          const uint8_t *restrict set, size_t setlen);
  /* First index in p[0..n) whose byte is a member of a 256-bit set, or
   * SIZE_MAX. `bitmap` is 32 bytes; byte v is a member iff
   * bitmap[v >> 3] & (1 << (v & 7)).
   *
   * find_first_of takes a LIST and is vectorised only to 8 elements; the common
   * character classes are far wider (\d is 10, [a-z] is 26, \w is 63), and a
   * list of that size falls back to a per-call membership table. This takes the
   * set already in bitmap form, so there is no per-call setup at all -- which
   * matters because the regexp prefilter calls it once per match ATTEMPT.
   *
   * Vectorised by two 16-byte shuffles: one selects the bitmap byte from
   * (v >> 3), the other turns (v & 7) into 1 << (v & 7). */
  size_t (*find_bitmap)(const uint8_t *restrict p, size_t n,
                        const uint8_t *restrict bitmap);
  /* Validate UTF-8: returns n if p[0..n) is well-formed UTF-8, else the byte
   * index of the first invalid sequence. SIMD ASCII fast path + scalar DFA. */
  size_t (*validate_utf8)(const uint8_t *restrict p, size_t n);

  /* ── base64 (RFC 4648, '+/' alphabet, '=' padding). ─────────────────── */
  /* Encode n bytes into dst (needs 4*ceil(n/3) bytes); returns bytes written. */
  size_t (*base64_encode)(const uint8_t *restrict src, size_t n,
                          char *restrict dst);
  /* Decode n base64 chars into dst (needs 3*(n/4) bytes); returns bytes
   * written, or SIZE_MAX on an invalid character / bad length. */
  size_t (*base64_decode)(const char *restrict src, size_t n,
                          uint8_t *restrict dst);

  /* ── hex (base16, lowercase output; RFC 4648). ──────────────────────── */
  /* Encode n bytes into 2n lowercase hex chars in dst. */
  void (*hex_encode)(const uint8_t *restrict src, size_t n,
                     char *restrict dst);
  /* Decode n hex chars (upper- or lower-case) into dst (n/2 bytes); returns
   * bytes written, or SIZE_MAX on an odd length or a non-hex character. */
  size_t (*hex_decode)(const char *restrict src, size_t n,
                       uint8_t *restrict dst);

  /* ── latin1 (ISO-8859-1) <-> UTF-8. ─────────────────────────────────── */
  /* Expand n latin1 bytes to UTF-8 in dst (needs up to 2n bytes); returns the
   * bytes written. Bytes <0x80 copy; 0x80..0xFF become a 2-byte sequence. */
  size_t (*latin1_to_utf8)(const uint8_t *restrict src, size_t n,
                           uint8_t *restrict dst);
  /* Narrow UTF-8 to latin1 in dst (needs up to n bytes); on success sets
   * *out_len to the bytes written and returns 0. Returns -1 (leaving *out_len
   * untouched) if the input is malformed UTF-8 or has any code point > 0xFF. */
  int (*utf8_to_latin1)(const uint8_t *restrict src, size_t n,
                        uint8_t *restrict dst, size_t *out_len);
  /* Count code points in valid UTF-8: bytes whose top two bits are not 10. */
  size_t (*count_utf8)(const uint8_t *restrict p, size_t n);

  /* ── UTF-8 <-> UTF-16 transcode + validation (simdutf lossless semantics).
   *    UTF-16 code units are host-endian uint16_t; on the little-endian targets
   *    this engine supports (x86-64, arm64) that is exactly UTF-16LE, hence the
   *    "le" in the kernel names. Transcoders are STRICT/lossless: they REJECT
   *    ill-formed input (no U+FFFD substitution), matching simdutf's non-"valid"
   *    convert path. Every fast-path block load is guarded by an i+width<=n bound
   *    so no read passes the input end. ─────────────────────────────────────── */
  /* Decode UTF-8 (src[0..n)) into UTF-16 code units in dst (needs up to n
   * units). On success sets *out_units and returns 0; returns -1 on malformed
   * UTF-8 (bad lead/continuation, overlong, surrogate code point, > U+10FFFF,
   * truncated). Valid UTF-8 never yields a lone surrogate, so the output of a
   * successful call is always well-formed UTF-16. */
  int (*utf8_to_utf16le)(const uint8_t *restrict src, size_t n,
                         uint16_t *restrict dst, size_t *out_units);
  /* Encode UTF-16 code units (src[0..units)) into UTF-8 in dst (needs up to
   * 3*units bytes). On success sets *out_len and returns 0; returns -1 on an
   * ill-formed surrogate (high not followed by low, lone/leading low, high at
   * end of buffer). */
  int (*utf16le_to_utf8)(const uint16_t *restrict src, size_t units,
                         uint8_t *restrict dst, size_t *out_len);
  /* Validate UTF-16: returns 1 if src[0..units) is well-formed (every high
   * surrogate immediately followed by a low surrogate, no lone low surrogate),
   * else 0. */
  int (*validate_utf16le)(const uint16_t *restrict src, size_t units);
  /* Count code points in UTF-16: units minus the number of low-surrogate units
   * (each surrogate PAIR is one code point). Equals the scalar-value count for
   * well-formed input; does not itself validate. */
  size_t (*count_utf16)(const uint16_t *restrict src, size_t units);

} simd_t;

/* Global dispatch table — safe to read from any thread after init. */
extern simd_t simd;

/* Initialize the dispatch table. Idempotent / thread-safe (uses once). */
void simd_init(void);

/* ══════════════════════════════════════════════════════════════════
 * Shared SIMD helpers (used across ISA backends)
 * ══════════════════════════════════════════════════════════════════ */

/* Horizontal sum of a float array using pairwise addition. */
static inline float simd_hsum_f32(const float *x, size_t n) {
  float acc = 0.0f;
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    acc += x[i] + x[i + 1] + x[i + 2] + x[i + 3] + x[i + 4] + x[i + 5] +
           x[i + 6] + x[i + 7];
  for (; i < n; i++)
    acc += x[i];
  return acc;
}

/* ══════════════════════════════════════════════════════════════════
 * Per-ISA override functions
 * ══════════════════════════════════════════════════════════════════ */
void simd_override_scalar(simd_t *t);
void simd_override_sse42(simd_t *t);
void simd_override_avx2(simd_t *t);
void simd_override_avx512(simd_t *t);
void simd_override_neon(simd_t *t);
void simd_override_sve(simd_t *t);

/* Shared helper: scalar top-k used by NEON and other ISAs. */
void simd_scalar_topk_indices(const float *restrict vals,
                                     uint32_t *restrict indices, size_t n,
                                     size_t k);

/* ══════════════════════════════════════════════════════════════════
 * Inline helpers
 * ══════════════════════════════════════════════════════════════════ */

/* Fast exponent approximation: Schraudolph's method.
 * e^x ≈ 2^(log2(e)·x) via IEEE-754 bit hack. ~1e-4 rel error for x∈[-88,88]. */
static inline float fast_exp(float x) {
  if (unlikely(x < -88.0f))
    return 0.0f;
  if (unlikely(x > 88.0f))
    return INFINITY;
  union {
    float f;
    int32_t i;
  } u;
  u.i = (int32_t)(12102203.0f * x + 1065353216.0f);
  return u.f;
}

static inline float fast_sigmoid(float x) {
  if (unlikely(x < -30.0f))
    return 0.0f;
  if (unlikely(x > 30.0f))
    return 1.0f;
  float ex = fast_exp(-x);
  return 1.0f / (1.0f + ex);
}

static inline float fast_tanh(float x) {
  if (unlikely(x < -10.0f))
    return -1.0f;
  if (unlikely(x > 10.0f))
    return 1.0f;
  return 2.0f * fast_sigmoid(2.0f * x) - 1.0f;
}

static inline bool is_pow2(size_t v) { return v && !(v & (v - 1)); }
static inline size_t align_up(size_t v, size_t a) {
  return (v + a - 1) & ~(a - 1);
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#endif /* DYNAJS_SIMD_KERNELS_H */