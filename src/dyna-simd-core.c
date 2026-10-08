#include "dyna-simd-kernels.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

simd_t simd;
simd_t simd_scalar;

#define DYN_TWOWAY_U8_AT(s, i) ((s)[i])
DYN_TWOWAY_DEFINE(dyn_twoway_u8, const uint8_t*, const uint8_t*, DYN_TWOWAY_U8_AT, DYN_TWOWAY_U8_AT)

int simd_strfind_fail(size_t* fails, size_t scanned, size_t m)
{
    return dyn_twoway_over_budget(++*fails, scanned, m);
}

size_t dyn_strfind_twoway_u8(const uint8_t* text, size_t n, const uint8_t* pat, size_t m)
{
    return dyn_twoway_u8(text, n, pat, m);
}
static pthread_once_t simd_once = PTHREAD_ONCE_INIT;

#if defined(__x86_64__)

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax,
    uint32_t* ebx, uint32_t* ecx, uint32_t* edx)
{
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

static inline uint64_t simd_xgetbv0(void)
{
#if defined(__GNUC__) || defined(__clang__)
    uint32_t eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (uint64_t)edx << 32 | eax;
#else
    return 0;
#endif
}

uint64_t cpu_features(void)
{
    uint64_t features = 0;
    uint32_t eax, ebx, ecx, edx, maxleaf;
    int os_ymm, os_zmm, has_fma;

    cpuid(0, 0, &maxleaf, &ebx, &ecx, &edx);
    if (maxleaf < 1)
        return features;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (ecx & (1u << 19))
        features |= CPU_SSE42;
    os_ymm = (ecx & (1u << 27)) && (simd_xgetbv0() & 0x6) == 0x6;
    os_zmm = (ecx & (1u << 27)) && (simd_xgetbv0() & 0xE6) == 0xE6;
    has_fma = (ecx & (1u << 12)) != 0;

    if (maxleaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & (1u << 5)) && os_ymm && has_fma)
            features |= CPU_AVX2;
        if ((ebx & (1u << 16)) && os_zmm)
            features |= CPU_AVX512F;
        if ((ebx & (1u << 17)) && os_zmm)
            features |= CPU_AVX512DQ;
        if ((ebx & (1u << 30)) && os_zmm)
            features |= CPU_AVX512BW;
    }

    if (!(features & CPU_AVX512F)) {
        features &= ~(uint64_t)(CPU_AVX512BW | CPU_AVX512DQ);
    }

    return features;
}

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

uint64_t cpu_features(void)
{
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

uint64_t cpu_features(void)
{
    uint64_t features = 0;
    features |= CPU_NEON;
    int has_sve = 0;
    size_t len = sizeof(has_sve);
    if (sysctlbyname("hw.optional.arm.FEAT_SVE", &has_sve, &len, NULL, 0) == 0) {
        if (has_sve)
            features |= CPU_SVE;
    }
    return features;
}

#else
uint64_t cpu_features(void) { return CPU_NEON; }
#endif

#else
uint64_t cpu_features(void) { return 0; }
#endif

#if defined(__x86_64__)
static inline float simd_hsum_128(__m128 v)
{
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    return _mm_cvtss_f32(v);
}
#endif

static void simd_init_impl(void)
{
    simd_override_scalar(&simd);
    simd_scalar = simd;

    uint64_t caps = cpu_features();

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

    if ((caps & (CPU_AVX512F | CPU_AVX512DQ | CPU_AVX512BW))
        == (CPU_AVX512F | CPU_AVX512DQ | CPU_AVX512BW))
        simd_override_avx512(&simd);
}

void simd_init(void)
{
    pthread_once(&simd_once, simd_init_impl);
}