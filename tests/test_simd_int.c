/*
 * test_simd_int.c -- regression + differential test for the shared SIMD
 * integer (i32) and inclusive prefix-scan kernels across every ISA in the
 * dispatch table:
 *   i32_sum, i32_min, i32_max, i32_dot, i32_add, i32_mul, i32_scale,
 *   f32_cumsum, i32_cumsum, f32_cummax, i32_cummax.
 *
 * Guards the two historical SIMD-kernel traps in this repo:
 *   1. HEAP-OOB READ/WRITE: a reduction that seeds a full SIMD-width vector
 *      BEFORE checking n reads past the buffer for 0 < n < width; a scan/
 *      elementwise store past out[n) writes past the buffer. Every input and
 *      output array is placed flush against an unmapped guard page, so any
 *      over-read OR over-write SIGSEGVs (works WITHOUT ASan -- important under
 *      qemu/Rosetta). Reductions early-return a scalar loop for small n; scans
 *      and elementwise ops only ever touch full blocks then a scalar tail.
 *   2. CORRECTNESS across ISAs and small/boundary/tail sizes, vs a sequential
 *      scalar reference, for n = 1..129, in-place and separate-out.
 *
 * Exactness (checked bit-for-bit, `==`, unless noted):
 *   - i32_sum (int64), i32_min/max (int32), i32_add/mul/scale (mod 2^32),
 *     i32_cumsum (mod 2^32), i32_cummax, f32_cummax: EXACT on every ISA.
 *   - i32_dot: sum of double products; the bounded differential data keeps every
 *     partial result <= 2^53, so double addition is exact regardless of the SIMD
 *     reduction order -> checked ==.
 *   - f32_cumsum: integer-valued differential data with partial sums < 2^24 is
 *     exact in float regardless of add order -> checked ==. A separate fractional
 *     sub-test checks the genuine reordered case within a relative tolerance.
 *
 * Build+run natively (arm64 NEON / x86 host). -ffp-contract=off keeps the C
 * reference non-fused:
 *   clang -O2 -ffp-contract=off -Isrc tests/test_simd_int.c \
 *     src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
 *     src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
 *     src/dyna-simd-sve.c -lpthread -lm -o /tmp/tsi && /tmp/tsi
 * Exercise the x86 SSE4.2 kernels under OrbStack/Rosetta (caps report SSE4.2):
 *   docker --context orbstack run --rm --platform linux/amd64 -v "$PWD":/src \
 *     -w /src dynascript-dev:amd64 bash -c '<build line above>'
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

/* ── sequential scalar references (the oracle) ─────────────────────── */
static int64_t ref_sum(const int32_t *x, size_t n) {
    int64_t a = 0;
    for (size_t i = 0; i < n; i++) a += (int64_t)x[i];
    return a;
}
static int32_t ref_min(const int32_t *x, size_t n) {
    int32_t m = INT32_MAX;
    for (size_t i = 0; i < n; i++) if (x[i] < m) m = x[i];
    return m;
}
static int32_t ref_max(const int32_t *x, size_t n) {
    int32_t m = INT32_MIN;
    for (size_t i = 0; i < n; i++) if (x[i] > m) m = x[i];
    return m;
}
static double ref_dot(const int32_t *a, const int32_t *b, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += (double)a[i] * (double)b[i];
    return s;
}
static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

static int fails = 0;
static long pg;
/* three guard regions: elementwise add/mul touch three arrays (a, b, out). */
static char *G[3];

/* an int32 buffer of length n flush against region r's guard page. */
static int32_t *bi(int r, size_t n) { return (int32_t *)(G[r] + pg - n * sizeof(int32_t)); }
static float   *bf(int r, size_t n) { return (float   *)(G[r] + pg - n * sizeof(float)); }

/* reorder-aware float tolerance for the fractional f32_cumsum sub-test. */
static int rel_ok(float got, float ref, double abscond) {
    double denom = abscond < 1.0 ? 1.0 : abscond;
    return fabs((double)got - (double)ref) <= 1e-4 * denom;
}

/* deterministic hashes: full-range int32, bounded int32, integer-valued float. */
static int32_t gen_full(size_t i, int trial, unsigned salt) {
    uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)trial * 40503u + salt * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (int32_t)h;
}
static int32_t gen_small(size_t i, int trial, unsigned salt) {
    uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)trial * 40503u + salt * 2246822519u;
    return (int32_t)(h % 4001u) - 2000; /* [-2000, 2000] -> dot stays exact */
}
static float gen_intf(size_t i, int trial, unsigned salt) {
    uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)trial * 40503u + salt * 2246822519u;
    return (float)((int)(h % 2001u) - 1000); /* integer [-1000,1000]: exact prefix */
}
static float gen_fracf(size_t i, int trial, unsigned salt) {
    uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)trial * 40503u + salt * 2246822519u;
    return (float)((int)(h % 4000u) - 2000) / 13.0f;
}

static void fail(const char *what, size_t n) {
    if (fails < 24) printf("  WRONG: %s n=%zu\n", what, n);
    fails++;
}

/* ── differential loop over guard-flush buffers, n = 1..129 ────────── */
static void differential(void) {
    int32_t expect[130];
    float fexpect[130];

    for (size_t n = 1; n <= 129; n++) {
        for (int trial = 0; trial < 20; trial++) {
            int32_t *A = bi(0, n), *B = bi(1, n), *O = bi(2, n);
            int32_t s = gen_full(0, trial, 9);

            /* ---- reductions (full-range, exact) ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen_full(i, trial, 1);
            if (simd.i32_sum(A, n) != ref_sum(A, n)) fail("i32_sum", n);
            if (simd.i32_min(A, n) != ref_min(A, n)) fail("i32_min", n);
            if (simd.i32_max(A, n) != ref_max(A, n)) fail("i32_max", n);

            /* ---- i32_dot (bounded so the double sum is exact) ---- */
            for (size_t i = 0; i < n; i++) { A[i] = gen_small(i, trial, 2); B[i] = gen_small(i, trial, 3); }
            if (simd.i32_dot(A, B, n) != ref_dot(A, B, n)) fail("i32_dot", n);

            /* ---- i32_add: separate out, then in-place (out == a) ---- */
            for (size_t i = 0; i < n; i++) { A[i] = gen_full(i, trial, 4); B[i] = gen_full(i, trial, 5); }
            simd.i32_add(O, A, B, n);
            for (size_t i = 0; i < n; i++) if (O[i] != wadd(A[i], B[i])) { fail("i32_add(out)", n); break; }
            for (size_t i = 0; i < n; i++) expect[i] = wadd(A[i], B[i]);
            simd.i32_add(A, A, B, n);
            for (size_t i = 0; i < n; i++) if (A[i] != expect[i]) { fail("i32_add(inplace)", n); break; }

            /* ---- i32_mul: separate out, then in-place (out == a) ---- */
            for (size_t i = 0; i < n; i++) { A[i] = gen_full(i, trial, 6); B[i] = gen_full(i, trial, 7); }
            simd.i32_mul(O, A, B, n);
            for (size_t i = 0; i < n; i++) if (O[i] != wmul(A[i], B[i])) { fail("i32_mul(out)", n); break; }
            for (size_t i = 0; i < n; i++) expect[i] = wmul(A[i], B[i]);
            simd.i32_mul(A, A, B, n);
            for (size_t i = 0; i < n; i++) if (A[i] != expect[i]) { fail("i32_mul(inplace)", n); break; }

            /* ---- i32_scale: separate out, then in-place ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen_full(i, trial, 8);
            simd.i32_scale(O, A, s, n);
            for (size_t i = 0; i < n; i++) if (O[i] != wmul(A[i], s)) { fail("i32_scale(out)", n); break; }
            for (size_t i = 0; i < n; i++) expect[i] = wmul(A[i], s);
            simd.i32_scale(A, A, s, n);
            for (size_t i = 0; i < n; i++) if (A[i] != expect[i]) { fail("i32_scale(inplace)", n); break; }

            /* ---- i32_cumsum (wrap): separate out, then in-place ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen_full(i, trial, 10);
            { uint32_t acc = 0; for (size_t i = 0; i < n; i++) { acc += (uint32_t)A[i]; expect[i] = (int32_t)acc; } }
            simd.i32_cumsum(O, A, n);
            for (size_t i = 0; i < n; i++) if (O[i] != expect[i]) { fail("i32_cumsum(out)", n); break; }
            simd.i32_cumsum(A, A, n);
            for (size_t i = 0; i < n; i++) if (A[i] != expect[i]) { fail("i32_cumsum(inplace)", n); break; }

            /* ---- i32_cummax: separate out, then in-place ---- */
            for (size_t i = 0; i < n; i++) A[i] = gen_full(i, trial, 11);
            { int32_t m = INT32_MIN; for (size_t i = 0; i < n; i++) { if (A[i] > m) m = A[i]; expect[i] = m; } }
            simd.i32_cummax(O, A, n);
            for (size_t i = 0; i < n; i++) if (O[i] != expect[i]) { fail("i32_cummax(out)", n); break; }
            simd.i32_cummax(A, A, n);
            for (size_t i = 0; i < n; i++) if (A[i] != expect[i]) { fail("i32_cummax(inplace)", n); break; }

            /* ---- f32_cumsum (integer-valued, exact): out then in-place ---- */
            float *FA = bf(0, n), *FO = bf(1, n);
            for (size_t i = 0; i < n; i++) FA[i] = gen_intf(i, trial, 12);
            { float acc = 0.0f; for (size_t i = 0; i < n; i++) { acc += FA[i]; fexpect[i] = acc; } }
            simd.f32_cumsum(FO, FA, n);
            for (size_t i = 0; i < n; i++) if (FO[i] != fexpect[i]) { fail("f32_cumsum(out)", n); break; }
            simd.f32_cumsum(FA, FA, n);
            for (size_t i = 0; i < n; i++) if (FA[i] != fexpect[i]) { fail("f32_cumsum(inplace)", n); break; }

            /* ---- f32_cummax (exact): out then in-place ---- */
            for (size_t i = 0; i < n; i++) FA[i] = gen_fracf(i, trial, 13);
            { float m = -INFINITY; for (size_t i = 0; i < n; i++) { if (FA[i] > m) m = FA[i]; fexpect[i] = m; } }
            simd.f32_cummax(FO, FA, n);
            for (size_t i = 0; i < n; i++) if (FO[i] != fexpect[i]) { fail("f32_cummax(out)", n); break; }
            simd.f32_cummax(FA, FA, n);
            for (size_t i = 0; i < n; i++) if (FA[i] != fexpect[i]) { fail("f32_cummax(inplace)", n); break; }

            /* ---- f32_cumsum fractional: reordered, within tolerance ---- */
            for (size_t i = 0; i < n; i++) FA[i] = gen_fracf(i, trial, 14);
            simd.f32_cumsum(FO, FA, n);
            { double acc = 0.0, abscond = 0.0;
              for (size_t i = 0; i < n; i++) { acc += (double)FA[i]; abscond += fabs((double)FA[i]);
                if (!rel_ok(FO[i], (float)acc, abscond)) { fail("f32_cumsum(frac)", n); break; } } }

            if (fails > 20) { printf("  ... too many failures, stopping\n"); return; }
        }
    }
}

/* ── hand vectors: overflow / negative / wrap / empty ──────────────── */
#define CK(cond, msg) do { if (!(cond)) { printf("  HAND WRONG: %s\n", msg); fails++; } } while (0)

static void hand_vectors(void) {
    /* i32_sum exceeds int32 but is exact in int64 */
    int32_t big[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    CK(simd.i32_sum(big, 3) == (int64_t)3 * INT32_MAX, "i32_sum > INT32_MAX");
    int32_t mix[4] = { INT32_MIN, INT32_MIN, 5, -5 };
    CK(simd.i32_sum(mix, 4) == (int64_t)2 * INT32_MIN, "i32_sum negatives");

    /* empty: kernels return the identity (the JS binding throws for min/max) */
    CK(simd.i32_sum(big, 0) == 0, "i32_sum empty = 0");
    CK(simd.i32_min(big, 0) == INT32_MAX, "i32_min empty = INT32_MAX");
    CK(simd.i32_max(big, 0) == INT32_MIN, "i32_max empty = INT32_MIN");
    CK(simd.i32_dot(big, big, 0) == 0.0, "i32_dot empty = 0");

    /* min / max with negatives and a single element */
    int32_t neg[5] = { -7, -3, -100, -3, -50 };
    CK(simd.i32_min(neg, 5) == -100, "i32_min negatives");
    CK(simd.i32_max(neg, 5) == -3, "i32_max negatives");
    int32_t one[1] = { -42 };
    CK(simd.i32_min(one, 1) == -42 && simd.i32_max(one, 1) == -42, "i32 min/max single");

    /* i32_add overflow wraps two's-complement */
    int32_t oa[3] = { INT32_MAX, INT32_MIN, -1 };
    int32_t ob[3] = { 1, -1, INT32_MIN };
    int32_t oo[3];
    simd.i32_add(oo, oa, ob, 3);
    CK(oo[0] == INT32_MIN && oo[1] == INT32_MAX && oo[2] == INT32_MAX, "i32_add overflow wrap");

    /* i32_mul low-32 (Math.imul) */
    int32_t ma[4] = { INT32_MAX, -2, 65536, -1 };
    int32_t mb[4] = { 2, 3, 65536, INT32_MIN };
    int32_t mo[4];
    simd.i32_mul(mo, ma, mb, 4);
    CK(mo[0] == -2 && mo[1] == -6 && mo[2] == 0 && mo[3] == INT32_MIN, "i32_mul low32");

    /* i32_scale = Math.imul(x, s) */
    int32_t sa[3] = { 3, -4, 1 << 30 };
    int32_t so[3];
    simd.i32_scale(so, sa, 7, 3);
    CK(so[0] == 21 && so[1] == -28 && so[2] == (int32_t)((uint32_t)(1u << 30) * 7u), "i32_scale");

    /* i32_cumsum overflow wrap */
    int32_t ca[4] = { INT32_MAX, 1, INT32_MAX, 2 };
    int32_t co[4];
    simd.i32_cumsum(co, ca, 4);
    CK(co[0] == INT32_MAX && co[1] == INT32_MIN &&
       co[2] == (int32_t)((uint32_t)INT32_MIN + (uint32_t)INT32_MAX) &&
       co[3] == (int32_t)((uint32_t)co[2] + 2u), "i32_cumsum wrap");

    /* i32_cummax with a mid dip and negatives */
    int32_t ka[6] = { -10, -3, -8, 5, 2, 5 };
    int32_t ko[6];
    simd.i32_cummax(ko, ka, 6);
    CK(ko[0] == -10 && ko[1] == -3 && ko[2] == -3 && ko[3] == 5 && ko[4] == 5 && ko[5] == 5, "i32_cummax");

    /* f32_cummax with negatives (identity -inf, not 0) */
    float fk[4] = { -3.0f, -5.0f, -1.0f, -2.0f };
    float fko[4];
    simd.f32_cummax(fko, fk, 4);
    CK(fko[0] == -3.0f && fko[1] == -3.0f && fko[2] == -1.0f && fko[3] == -1.0f, "f32_cummax negatives");

    /* f32_cumsum small exact */
    float fc[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    float fco[5];
    simd.f32_cumsum(fco, fc, 5);
    CK(fco[0] == 1 && fco[1] == 3 && fco[2] == 6 && fco[3] == 10 && fco[4] == 15, "f32_cumsum exact");
}

int main(void) {
    simd_init();
    unsigned long long c = cpu_features();
    printf("caps=0x%llx SSE42=%d AVX2=%d AVX512=%d NEON=%d SVE=%d\n", c,
           !!(c & CPU_SSE42), !!(c & CPU_AVX2), !!(c & CPU_AVX512F),
           !!(c & CPU_NEON), !!(c & CPU_SVE));

    pg = sysconf(_SC_PAGESIZE);
    for (int r = 0; r < 3; r++) {
        G[r] = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (G[r] == MAP_FAILED) { perror("mmap"); return 2; }
        if (mprotect(G[r] + pg, pg, PROT_NONE)) { perror("mprotect"); return 2; }
    }

    differential();
    hand_vectors();

    if (fails == 0)
        printf("OK: i32 + prefix-scan kernels correct + no guard-page fault (no OOB), n=1..129\n");
    else
        printf("FAILS=%d\n", fails);
    return fails ? 1 : 0;
}
