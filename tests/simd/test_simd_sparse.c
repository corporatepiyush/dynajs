/*
 * test_simd_sparse.c -- the sparse kernels behind dyna:simd gatherAdd,
 * f64GatherAdd, spmv and f64Spmv (src/core/simdk/simdk-scalar.c), standalone.
 *
 * Oracle: naive loops written here. gatherAdd must add (not store) the
 * gathered values, repeated indices included; spmv must give each CSR row's
 * sum in row-pointer order, accumulated in double for both widths (f32
 * rounds once at the end), with empty rows giving 0; the product is rounded
 * before the add (no fused multiply-add), in the kernel and in the oracle. Seeded random matrices
 * with 0..12 entries per row, so empty rows and repeated columns occur.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/core/dyn-simdk.h"

static int fails, checks;
static uint64_t rng = 0x243F6A8885A308D3ULL;
static uint32_t rnd(uint32_t k)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng % k);
}

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static void test_gather(void)
{
    enum { NS = 97, NI = 300 };
    double s64[NS], d64[NI], w64[NI];
    float s32[NS], d32[NI], w32[NI];
    int32_t idx[NI];
    size_t i;
    for (i = 0; i < NS; i++) {
        s64[i] = (double)rnd(1000) / 7.0;
        s32[i] = (float)rnd(1000) / 3.0f;
    }
    for (i = 0; i < NI; i++) {
        idx[i] = (int32_t)rnd(NS);
        d64[i] = w64[i] = (double)rnd(50);
        d32[i] = w32[i] = (float)rnd(50);
        w64[i] += s64[idx[i]];
        w32[i] += s32[idx[i]];
    }
    simd_gather_add_f64(d64, s64, idx, NI);
    simd_gather_add_f32(d32, s32, idx, NI);
    CHECK(memcmp(d64, w64, sizeof d64) == 0, "f64 gather-add differs from dst[i] += src[idx[i]]");
    CHECK(memcmp(d32, w32, sizeof d32) == 0, "f32 gather-add differs from dst[i] += src[idx[i]]");
}

static void test_spmv(void)
{
    enum { NY = 200, NX = 64 };
    int32_t rp[NY + 1], *col = malloc(NY * 12 * sizeof *col);
    double *v64 = malloc(NY * 12 * sizeof *v64), x64[NX], y64[NY];
    float *v32 = malloc(NY * 12 * sizeof *v32), x32[NX], y32[NY];
    size_t r, k, nnz = 0, empty = 0;
    int it;
    for (it = 0; it < 20; it++) {
        nnz = 0;
        rp[0] = 0;
        for (r = 0; r < NY; r++) {
            uint32_t m = rnd(13);
            empty += m == 0;
            for (k = 0; k < m; k++, nnz++) {
                col[nnz] = (int32_t)rnd(NX);
                v64[nnz] = (double)rnd(2001) / 1000.0 - 1.0;
                v32[nnz] = (float)v64[nnz];
            }
            rp[r + 1] = (int32_t)nnz;
        }
        for (k = 0; k < NX; k++) {
            x64[k] = (double)rnd(2001) / 997.0 - 1.0;
            x32[k] = (float)x64[k];
        }
        for (r = 0; r < NY; r++)
            y64[r] = y32[r] = 12345.0f;
        simd_spmv_f64(y64, NY, v64, col, rp, x64);
        simd_spmv_f32(y32, NY, v32, col, rp, x32);
        for (r = 0; r < NY; r++) {
            double s = 0.0, t = 0.0;
            for (k = (size_t)rp[r]; k < (size_t)rp[r + 1]; k++) {
                double p = v64[k] * x64[col[k]], q = (double)v32[k] * (double)x32[col[k]];
                s += p;
                t += q;
            }
            CHECK(y64[r] == s, "f64 row %zu: %.17g want %.17g", r, y64[r], s);
            CHECK(y32[r] == (float)t, "f32 row %zu: %.9g want %.9g", r, (double)y32[r], (double)(float)t);
        }
    }
    CHECK(empty > 0, "no empty rows generated");
    free(col);
    free(v64);
    free(v32);
}

int main(void)
{
    test_gather();
    test_spmv();
    printf("test_simd_sparse: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
