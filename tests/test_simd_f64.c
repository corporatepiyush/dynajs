/*
 * test_simd_f64.c -- regression + differential test for the shared SIMD
 * double-precision (f64) array kernels across every ISA in the dispatch table:
 *   f64_sum, f64_dot, f64_min, f64_max, f64_scale, f64_axpy.
 *
 * Guards the two historical SIMD-kernel traps in this repo:
 *   1. HEAP-OOB READ: a reduction that seeds a full SIMD-width vector
 *      (vld1q_f64/_mm_loadu_pd/_mm256_loadu_pd/_mm512_loadu_pd/svld1) BEFORE
 *      checking n reads past the buffer for 0 < n < width. Every f64 reduction
 *      here early-returns a scalar loop for small n; this test proves it.
 *   2. CORRECTNESS across ISAs and small/boundary/tail sizes.
 *
 * Method: place exactly n doubles flush against an unmapped guard page, so any
 * read past x[n) SIGSEGVs (works WITHOUT ASan -- important under qemu). Compare
 * every kernel to a sequential scalar reference for n = 1..129.
 *   - f64_sum / f64_dot REORDER additions vs a sequential loop, so they are NOT
 *     bit-identical: checked with a RELATIVE-ERROR tolerance (1e-12).
 *   - f64_min / f64_max / f64_scale / f64_axpy ARE exact: checked bit-for-bit.
 *     (axpy is a non-fused mul-then-add on every ISA, matching the scalar ref.)
 *
 * Build+run natively (arm64 NEON / x86 host). -ffp-contract=off keeps the C
 * reference non-fused so the exact axpy check is valid on every compiler:
 *   clang -O2 -ffp-contract=off -Isrc tests/test_simd_f64.c \
 *     src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
 *     src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
 *     src/dyna-simd-sve.c -lpthread -lm -o /tmp/tsf64 && /tmp/tsf64
 * Exercise the x86 kernels under emulation (build into /tmp, never host .obj):
 *   docker run --rm --platform linux/amd64 -e QEMU_CPU=Haswell -v "$PWD":/src \
 *     -w /src dynascript-dev:amd64 bash -c '<build line above>'
 *   (QEMU_CPU: Nehalem => SSE2, Haswell => AVX, Skylake-Server => AVX-512.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

static double ref_sum(const double *x, size_t n) {
    double a = 0.0;
    for (size_t i = 0; i < n; i++) a += x[i];
    return a;
}
static double ref_dot(const double *a, const double *b, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}
static double ref_max(const double *x, size_t n) {
    double m = x[0];
    for (size_t i = 1; i < n; i++) if (x[i] > m) m = x[i];
    return m;
}
static double ref_min(const double *x, size_t n) {
    double m = x[0];
    for (size_t i = 1; i < n; i++) if (x[i] < m) m = x[i];
    return m;
}

/* strict bit-for-bit equality (no NaN in the test data, so this is exactness) */
static int d_bits_eq(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

/* reordered-reduction tolerance: |got-ref| / max(1,|ref|) <= 1e-12 */
static int rel_ok(double got, double ref) {
    double denom = fabs(ref) < 1.0 ? 1.0 : fabs(ref);
    return fabs(got - ref) / denom <= 1e-12;
}

/* deterministic pseudo-random double in ~[-153, 153] with a fractional part */
static double gen(size_t i, int trial, unsigned salt) {
    unsigned h = (unsigned)i * 2654435761u + (unsigned)trial * 40503u + salt * 2246822519u;
    return (double)((int)(h % 4000u) - 2000) / 13.0;
}

int main(void) {
    simd_init();
    unsigned long long c = cpu_features();
    printf("caps=0x%llx SSE42=%d AVX2=%d AVX512=%d NEON=%d SVE=%d\n", c,
           !!(c & CPU_SSE42), !!(c & CPU_AVX2), !!(c & CPU_AVX512F),
           !!(c & CPU_NEON), !!(c & CPU_SVE));

    long pg = sysconf(_SC_PAGESIZE);
    /* two guard regions: some kernels touch two arrays (a/b, x/out, y/x). */
    char *g0 = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *g1 = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g0 == MAP_FAILED || g1 == MAP_FAILED) { perror("mmap"); return 2; }
    if (mprotect(g0 + pg, pg, PROT_NONE) || mprotect(g1 + pg, pg, PROT_NONE)) {
        perror("mprotect"); return 2;
    }

    int fails = 0;
    double yorig[130], expect[130];

    for (size_t n = 1; n <= 129; n++) {
        for (int trial = 0; trial < 24; trial++) {
            /* arrays flush against the guard page: any read past [n) faults */
            double *A = (double *)(g0 + pg - n * sizeof(double));
            double *B = (double *)(g1 + pg - n * sizeof(double));
            double s = ((trial % 7) - 3) * 0.5 + 1.25; /* scalar for scale/axpy */

            /* ---- f64_sum (relative) ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen(i, trial, 1);
            if (!rel_ok(simd.f64_sum(A, n), ref_sum(A, n))) { printf("f64_sum n=%zu WRONG\n", n); fails++; }

            /* ---- f64_max / f64_min (exact) ---- */
            if (!d_bits_eq(simd.f64_max(A, n), ref_max(A, n))) { printf("f64_max n=%zu WRONG\n", n); fails++; }
            if (!d_bits_eq(simd.f64_min(A, n), ref_min(A, n))) { printf("f64_min n=%zu WRONG\n", n); fails++; }

            /* ---- f64_dot (relative), a=A flush g0, b=B flush g1 ---- */
            for (size_t i = 0; i < n; i++) B[i] = gen(i, trial, 2);
            if (!rel_ok(simd.f64_dot(A, B, n), ref_dot(A, B, n))) { printf("f64_dot n=%zu WRONG\n", n); fails++; }

            /* ---- f64_scale, separate out (x=A flush g0, out=B flush g1) exact ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen(i, trial, 3);
            simd.f64_scale(B, A, s, n);
            for (size_t i = 0; i < n; i++)
                if (!d_bits_eq(B[i], A[i] * s)) { printf("f64_scale(out) n=%zu i=%zu WRONG\n", n, i); fails++; break; }
            /* ---- f64_scale, in-place (out == x) exact ---- */
            for (size_t i = 0; i < n; i++) expect[i] = A[i] * s;
            simd.f64_scale(A, A, s, n);
            for (size_t i = 0; i < n; i++)
                if (!d_bits_eq(A[i], expect[i])) { printf("f64_scale(inplace) n=%zu i=%zu WRONG\n", n, i); fails++; break; }

            /* ---- f64_axpy: y=A flush g0, x=B flush g1, exact non-fused ---- */
            for (size_t i = 0; i < n; i++) { A[i] = gen(i, trial, 4); B[i] = gen(i, trial, 5); yorig[i] = A[i]; }
            simd.f64_axpy(A, s, B, n);
            for (size_t i = 0; i < n; i++) {
                double p = s * B[i];           /* non-fused, matches the kernel */
                double want = yorig[i] + p;
                if (!d_bits_eq(A[i], want)) { printf("f64_axpy n=%zu i=%zu WRONG\n", n, i); fails++; break; }
            }

            if (fails > 20) { printf("... too many failures, stopping\n"); goto done; }
        }
    }
done:
    if (fails == 0)
        printf("OK: f64 sum/dot/min/max/scale/axpy correct + no guard-page fault (no OOB), n=1..129\n");
    else
        printf("FAILS=%d\n", fails);
    return fails ? 1 : 0;
}
