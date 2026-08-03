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

/* SIMD dispatch core: CPU feature detection, dispatch table, init.
 * The per-ISA .c files (avx2, avx512, neon, sve) provide override
 * functions that selectively fill in the dispatch table.
 *
 * Coverage: x86-64 (CPUID), ARM64 Linux (getauxval), ARM64 macOS (sysctl).
 */

#include "dyna-simd-kernels.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Global dispatch table — always fully populated via scalar fallback first. */
simd_t simd;
static pthread_once_t simd_once = PTHREAD_ONCE_INIT;

/* ══════════════════════════════════════════════════════════════════
 * x86-64 CPUID-based feature detection
 * ══════════════════════════════════════════════════════════════════ */

#if defined(__x86_64__)

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                                uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ volatile("cpuid"
                   : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                   : "a"(leaf), "c"(subleaf)
                   : "memory");
#else
  *eax = *ebx = *ecx = *edx = 0;
  (void)leaf;
  (void)subleaf;
#endif
}

uint64_t cpu_features(void) {
  uint64_t features = 0;
  uint32_t eax, ebx, ecx, edx;

  /* Leaf 1: detect SSE4.2 */
  cpuid(1, 0, &eax, &ebx, &ecx, &edx);
  if (ecx & (1u << 19))
    features |= CPU_SSE42;

  /* Leaf 7, subleaf 0: detect AVX2 and AVX-512 */
  cpuid(7, 0, &eax, &ebx, &ecx, &edx);
  if (ebx & (1u << 5))
    features |= CPU_AVX2;
  if (ebx & (1u << 16))
    features |= CPU_AVX512F;
  if (ebx & (1u << 17))
    features |= CPU_AVX512DQ;
  if (ebx & (1u << 30))
    features |= CPU_AVX512BW;

  /* Sanity: AVX-512 sub-features require AVX-512F */
  if (!(features & CPU_AVX512F)) {
    features &= ~(uint64_t)(CPU_AVX512BW | CPU_AVX512DQ);
  }

  return features;
}

/* ══════════════════════════════════════════════════════════════════
 * ARM64 feature detection
 * ══════════════════════════════════════════════════════════════════ */

#elif defined(__aarch64__)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef AT_HWCAP
#define AT_HWCAP 16
#endif
#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD (1 << 1)
#endif
#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

uint64_t cpu_features(void) {
  uint64_t features = 0;
  unsigned long hwcap = getauxval(AT_HWCAP);
  if (hwcap & HWCAP_ASIMD)
    features |= CPU_NEON;
  if (hwcap & HWCAP_SVE)
    features |= CPU_SVE;
  return features;
}

#elif defined(__APPLE__)
#include <sys/sysctl.h>

uint64_t cpu_features(void) {
  uint64_t features = 0;
  /* All Apple Silicon has NEON. */
  features |= CPU_NEON;
  /* Check for SVE (M1 Ultra / M2 and later have SVE). */
  int has_sve = 0;
  size_t len = sizeof(has_sve);
  if (sysctlbyname("hw.optional.arm.FEAT_SVE", &has_sve, &len, NULL, 0) == 0) {
    if (has_sve)
      features |= CPU_SVE;
  }
  return features;
}

#else
/* Generic ARM64 fallback — assume NEON (all ARMv8+ have it). */
uint64_t cpu_features(void) { return CPU_NEON; }
#endif

/* ══════════════════════════════════════════════════════════════════
 * Other architectures — no SIMD features reported
 * ══════════════════════════════════════════════════════════════════ */

#else
uint64_t cpu_features(void) { return 0; }
#endif

/* ══════════════════════════════════════════════════════════════════
 * shared SIMD helpers (used across multiple ISA backends)
 * ══════════════════════════════════════════════════════════════════ */

/* Blade: pairwise reduction 4→1 within a 128-bit lane (x86 only). */
#if defined(__x86_64__)
static inline float simd_hsum_128(__m128 v) {
  v = _mm_add_ps(v, _mm_movehl_ps(v, v));
  v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
  return _mm_cvtss_f32(v);
}
#endif

/* ══════════════════════════════════════════════════════════════════
 * Dispatch initialization
 * Priority (applied lowest-first, later overrides replace):
 *   Scalar → SSE4.2 → NEON → AVX2+FMA → AVX-512 → SVE
 * The final result holds the best available slot for each kernel.
 * ══════════════════════════════════════════════════════════════════ */

static void simd_init_impl(void) {
  /* 1. Start with the universal scalar fallback. */
  simd_override_scalar(&simd);

  /* 2. Detect features. */
  uint64_t caps = cpu_features();

  /* 3. Install best available — order matters.
   *    Each override only fills non-NULL slots,
   *    so higher-ISA slots overwrite lower ones. */
  if (caps & CPU_SSE42)
    simd_override_sse42(&simd);

  if (caps & CPU_NEON)
    simd_override_neon(&simd);

#if defined(__ARM_FEATURE_SVE)
  if (caps & CPU_SVE)
    simd_override_sve(&simd);
#endif

  if (caps & CPU_AVX2)
    simd_override_avx2(&simd);

  if ((caps & CPU_AVX512F) == CPU_AVX512F)
    simd_override_avx512(&simd);
}

void simd_init(void) {
  pthread_once(&simd_once, simd_init_impl);
}