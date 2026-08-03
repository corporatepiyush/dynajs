/*
 * test_simd_reductions.c -- regression test for the shared SIMD reduction
 * kernels (max/min/argmax/argmin/argminmax) across every ISA in the dispatch
 * table. Guards two historical bugs:
 *   1. HEAP-OOB READ: the accelerated kernels seeded a full SIMD-width vector
 *      (vld1q/_mm_loadu/_mm256_loadu/_mm512_loadu/svld1) BEFORE checking n, so
 *      0 < n < width read past the buffer. Fixed by an n<width scalar guard.
 *   2. Correctness of the small-n and boundary sizes.
 *
 * Method: place exactly n floats flush against an unmapped guard page, so any
 * read past x[n) SIGSEGVs (works WITHOUT ASan -- important under qemu, where
 * ASan is unreliable). Compare every kernel to a scalar reference for n=1..129.
 *
 * Build+run natively (arm64 NEON / x86 host):
 *   clang -O2 -Isrc tests/test_simd_reductions.c \
 *     src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
 *     src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
 *     src/dyna-simd-sve.c -lpthread -lm -o /tmp/tsr && /tmp/tsr
 * Exercise the x86 kernels under emulation (Haswell => AVX2):
 *   docker run --rm --platform linux/amd64 -e QEMU_CPU=Haswell -v "$PWD":/src \
 *     -w /src dynascript-dev:amd64 bash -c 'make clean >/dev/null; <build line above>; /tmp/tsr'
 * Optionally also under ASan natively for redzone coverage of the tail loops.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

static float ref_max(const float *x, size_t n) { float m = x[0]; for (size_t i = 1; i < n; i++) if (x[i] > m) m = x[i]; return m; }
static float ref_min(const float *x, size_t n) { float m = x[0]; for (size_t i = 1; i < n; i++) if (x[i] < m) m = x[i]; return m; }
static size_t ref_amax(const float *x, size_t n) { size_t k = 0; for (size_t i = 1; i < n; i++) if (x[i] > x[k]) k = i; return k; }
static size_t ref_amin(const float *x, size_t n) { size_t k = 0; for (size_t i = 1; i < n; i++) if (x[i] < x[k]) k = i; return k; }

int main(void)
{
    simd_init();
    unsigned long long c = cpu_features();
    printf("caps=0x%llx SSE42=%d AVX2=%d AVX512=%d NEON=%d\n", c,
           !!(c & CPU_SSE42), !!(c & CPU_AVX2), !!(c & CPU_AVX512F), !!(c & CPU_NEON));

    long pg = sysconf(_SC_PAGESIZE);
    char *base = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 2; }
    if (mprotect(base + pg, pg, PROT_NONE) != 0) { perror("mprotect"); return 2; }

    int fails = 0;
    /* n up to 129 covers every ISA width (AVX-512=16) plus multi-vector + tail
     * plus the binding's n<64 scalar/kernel switch boundary. */
    for (size_t n = 1; n <= 129; n++) {
        for (int trial = 0; trial < 40; trial++) {
            /* x[n] lands exactly on the guard page: any read past x[n) faults */
            float *x = (float *)(base + pg - n * sizeof(float));
            for (size_t i = 0; i < n; i++)
                x[i] = (float)((int)((i * 2654435761u + trial * 40503u) % 4000) - 2000) / 13.0f;
            if (simd.max(x, n) != ref_max(x, n)) { printf("max n=%zu WRONG\n", n); fails++; }
            if (simd.min(x, n) != ref_min(x, n)) { printf("min n=%zu WRONG\n", n); fails++; }
            /* arg*: compare by VALUE at the returned index (ties are allowed) */
            if (x[simd.argmax(x, n)] != x[ref_amax(x, n)]) { printf("argmax n=%zu WRONG\n", n); fails++; }
            if (x[simd.argmin(x, n)] != x[ref_amin(x, n)]) { printf("argmin n=%zu WRONG\n", n); fails++; }
            size_t am, ax;
            simd.argminmax(x, n, &am, &ax);
            if (x[am] != x[ref_amin(x, n)] || x[ax] != x[ref_amax(x, n)]) { printf("argminmax n=%zu WRONG\n", n); fails++; }
            if (fails > 20) { printf("... too many failures, stopping\n"); goto done; }
        }
    }
done:
    if (fails == 0)
        printf("OK: max/min/argmax/argmin/argminmax correct + no guard-page fault (no OOB), n=1..129\n");
    else
        printf("FAILS=%d\n", fails);
    return fails ? 1 : 0;
}
