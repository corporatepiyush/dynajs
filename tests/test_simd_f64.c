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

static int d_bits_eq(double a, double b) { return memcmp(&a, &b, sizeof a) == 0; }

static int rel_ok(double got, double ref) {
    double denom = fabs(ref) < 1.0 ? 1.0 : fabs(ref);
    return fabs(got - ref) / denom <= 1e-12;
}

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
            double *A = (double *)(g0 + pg - n * sizeof(double));
            double *B = (double *)(g1 + pg - n * sizeof(double));
            double s = ((trial % 7) - 3) * 0.5 + 1.25;

            for (size_t i = 0; i < n; i++) A[i] = gen(i, trial, 1);
            if (!rel_ok(simd.f64_sum(A, n), ref_sum(A, n))) { printf("f64_sum n=%zu WRONG\n", n); fails++; }

            if (!d_bits_eq(simd.f64_max(A, n), ref_max(A, n))) { printf("f64_max n=%zu WRONG\n", n); fails++; }
            if (!d_bits_eq(simd.f64_min(A, n), ref_min(A, n))) { printf("f64_min n=%zu WRONG\n", n); fails++; }

            for (size_t i = 0; i < n; i++) B[i] = gen(i, trial, 2);
            if (!rel_ok(simd.f64_dot(A, B, n), ref_dot(A, B, n))) { printf("f64_dot n=%zu WRONG\n", n); fails++; }

            for (size_t i = 0; i < n; i++) A[i] = gen(i, trial, 3);
            simd.f64_scale(B, A, s, n);
            for (size_t i = 0; i < n; i++)
                if (!d_bits_eq(B[i], A[i] * s)) { printf("f64_scale(out) n=%zu i=%zu WRONG\n", n, i); fails++; break; }
            for (size_t i = 0; i < n; i++) expect[i] = A[i] * s;
            simd.f64_scale(A, A, s, n);
            for (size_t i = 0; i < n; i++)
                if (!d_bits_eq(A[i], expect[i])) { printf("f64_scale(inplace) n=%zu i=%zu WRONG\n", n, i); fails++; break; }

            for (size_t i = 0; i < n; i++) { A[i] = gen(i, trial, 4); B[i] = gen(i, trial, 5); yorig[i] = A[i]; }
            simd.f64_axpy(A, s, B, n);
            for (size_t i = 0; i < n; i++) {
                double p = s * B[i];
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
