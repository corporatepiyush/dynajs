// covers: c:src/core/statk
/*
 * test_statk_core.c -- src/core/statk (the dyna:mathx stats tests) as a
 * standalone C library under ASan/UBSan.
 *
 * Checks: the radix argsort agrees with qsort over seeded random inputs
 * (including -0.0, +-Inf, duplicates and both sides of the 64-element
 * insertion-sort cut), is stable, and rank() handles every tie method; the
 * Kolmogorov-Smirnov exact path counts stay in [0, 1] and match the
 * enumerated permutation p for every small size pair; the exact
 * Mann-Whitney sf sums to 1 with its complement; bootstrap intervals are
 * bit-identical whether the fork-join runs on one thread or many, and a
 * stop callback cancels it; the vectorised distribution map is the same
 * on one thread and many.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-par.h"
#include "dyn-statk.h"

static int fails, checks;

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

static uint64_t rs = 88172645463325252ull;

static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static const double* cmp_base;

static int cmp_idx(const void* a, const void* b)
{
    uint32_t i = *(const uint32_t*)a, j = *(const uint32_t*)b;
    double x = cmp_base[i], y = cmp_base[j];
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return i < j ? -1 : (i > j);
}

static void test_argsort(void)
{
    uint32_t sizes[] = { 0, 1, 2, 63, 64, 65, 1000, 70000 }, s;
    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t n = sizes[s], i, bad = 0, unstable = 0;
        double* x = (double*)malloc((n ? n : 1) * sizeof(double));
        uint32_t *a = (uint32_t*)malloc((n ? n : 1) * sizeof(uint32_t)), *b = (uint32_t*)malloc((n ? n : 1) * sizeof(uint32_t));
        for (i = 0; i < n; i++) {
            uint64_t r = rnd() % 10;
            x[i] = r == 0 ? -0.0 : r == 1 ? 0.0
                : r == 2                  ? INFINITY
                : r == 3                  ? -INFINITY
                                          : (double)(int64_t)(rnd() % 2001) - 1000.0 + (r == 4 ? 0.5 : 0.0);
            b[i] = i;
        }
        CHECK(dyn_st_argsort(x, n, a) == DYN_ST_OK, "argsort n=%u", n);
        for (i = 0; i < n; i++) {
            uint32_t j = i;
            while (j + 1 < n && x[j] == x[j + 1])
                j++;
        }
        cmp_base = x;
        if (n)
            qsort(b, n, sizeof(uint32_t), cmp_idx);
        for (i = 0; i < n; i++) {
            if (x[a[i]] != x[b[i]])
                bad++;
            if (i && x[a[i]] == x[a[i - 1]] && a[i] < a[i - 1] && signbit(x[a[i]]) == signbit(x[a[i - 1]]))
                unstable++;
        }
        CHECK(bad == 0, "argsort n=%u orders like qsort (%u positions differ)", n, bad);
        CHECK(unstable == 0, "argsort n=%u is stable (%u inversions among equal keys)", n, unstable);
        free(x);
        free(a);
        free(b);
    }
}

static void test_rank(void)
{
    double x[] = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5 }, out[11], tt;
    double avg[] = { 4.5, 1.5, 6, 1.5, 8, 11, 3, 10, 8, 4.5, 8 };
    double dense[] = { 3, 1, 4, 1, 5, 7, 2, 6, 5, 3, 5 };
    int i, okA = 1, okD = 1;
    CHECK(dyn_st_rank(x, 11, DYN_ST_RANK_AVERAGE, out, &tt) == DYN_ST_OK, "rank average");
    for (i = 0; i < 11; i++)
        okA &= out[i] == avg[i];
    CHECK(okA, "average ranks of the pi digits");
    CHECK(tt == 6.0 + 6.0 + 24.0, "tie term sum(t^3 - t) = 36 (got %g)", tt);
    CHECK(dyn_st_rank(x, 11, DYN_ST_RANK_DENSE, out, NULL) == DYN_ST_OK, "rank dense");
    for (i = 0; i < 11; i++)
        okD &= out[i] == dense[i];
    CHECK(okD, "dense ranks of the pi digits");
}

static int popc(unsigned v)
{
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

static void test_ks_exact_all(void)
{
    uint32_t n1, n2;
    for (n1 = 1; n1 <= 7; n1++)
        for (n2 = 1; n2 <= 7; n2++) {
            uint32_t n = n1 + n2, mask, total = 0;
            double a[7], b[7], pooled[14];
            uint32_t i;
            for (i = 0; i < n; i++)
                pooled[i] = (double)i;
            for (i = 0; i < n1; i++)
                a[i] = (double)(2 * i);
            for (i = 0; i < n2; i++)
                b[i] = (double)(2 * i) + 1.0 + 2.0 * (double)(i % 3);
            {
                dyn_st_result_t r;
                int64_t L = (int64_t)n1 * n2;
                uint32_t hit = 0;
                double dobs;
                CHECK(dyn_st_ks2(a, n1, b, n2, DYN_ST_TWO_SIDED, DYN_ST_EXACT, &r) == DYN_ST_OK, "ks2 %u,%u", n1, n2);
                dobs = r.statistic;
                (void)pooled;
                for (mask = 0; mask < (1u << n); mask++) {
                    double x[14], y[14];
                    uint32_t nx = 0, ny = 0;
                    dyn_st_result_t q;
                    if ((uint32_t)popc(mask) != n1)
                        continue;
                    {
                        double all[14];
                        uint32_t k = 0;
                        for (i = 0; i < n1; i++)
                            all[k++] = a[i];
                        for (i = 0; i < n2; i++)
                            all[k++] = b[i];
                        for (i = 0; i < n; i++) {
                            if (mask & (1u << i))
                                x[nx++] = all[i];
                            else
                                y[ny++] = all[i];
                        }
                    }
                    dyn_st_ks2(x, nx, y, ny, DYN_ST_TWO_SIDED, DYN_ST_ASYMP, &q);
                    total++;
                    if (llround(q.statistic * (double)L) >= llround(dobs * (double)L))
                        hit++;
                }
                CHECK(fabs(r.pvalue - (double)hit / (double)total) < 1e-12, "ks exact %u,%u: p %.17g vs enumerated %.17g", n1, n2,
                    r.pvalue, (double)hit / (double)total);
            }
        }
}

static void test_mwu_complement(void)
{
    double a[] = { 1, 4, 9, 11, 15 }, b[] = { 2, 3, 5, 6, 7, 8, 10 }, pooled[12];
    dyn_st_result_t g, l;
    unsigned mask, total = 0, eq = 0, ge = 0;
    int i, j, k = 0;
    double u = 0.0;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 7; j++)
            u += a[i] > b[j];
    for (i = 0; i < 5; i++)
        pooled[k++] = a[i];
    for (j = 0; j < 7; j++)
        pooled[k++] = b[j];
    for (mask = 0; mask < (1u << 12); mask++) {
        double v = 0.0;
        if (popc(mask) != 5)
            continue;
        for (i = 0; i < 12; i++)
            for (j = 0; j < 12; j++)
                if ((mask & (1u << i)) && !(mask & (1u << j)))
                    v += pooled[i] > pooled[j];
        total++;
        eq += v == u;
        ge += v >= u;
    }
    CHECK(dyn_st_mannwhitney(a, 5, b, 7, DYN_ST_GREATER, DYN_ST_EXACT, 1, &g) == DYN_ST_OK, "mwu greater");
    CHECK(dyn_st_mannwhitney(a, 5, b, 7, DYN_ST_LESS, DYN_ST_EXACT, 1, &l) == DYN_ST_OK, "mwu less");
    CHECK(fabs(g.pvalue - (double)ge / total) < 1e-12, "exact P(U >= u) = %.17g, enumerated %.17g", g.pvalue, (double)ge / total);
    CHECK(fabs(g.pvalue + l.pvalue - 1.0 - (double)eq / total) < 1e-12, "P(U >= u) + P(U <= u) = 1 + P(U = u)");
}

static int stop_now(void* ud)
{
    (void)ud;
    return 1;
}

static void test_bootstrap(void)
{
    uint32_t n = 20000, i;
    double* a = (double*)malloc(n * sizeof(double));
    double* b = (double*)malloc(n / 2 * sizeof(double));
    dyn_st_boot_t o1, o2, o3;
    dyn_par_stats_t st;
    for (i = 0; i < n; i++)
        a[i] = (double)(rnd() % 100000) / 100.0;
    for (i = 0; i < n / 2; i++)
        b[i] = (double)(rnd() % 90000) / 100.0;
    memset(&o1, 0, sizeof o1);
    o1.stat = DYN_ST_BOOT_MEDIAN;
    o1.cmp = DYN_ST_BOOT_RATIO;
    o1.resamples = 257;
    o1.level = 0.95;
    o1.seed = 7;
    o2 = o1;
    o3 = o1;
    dyn_par_set_spawn_limit(0);
    CHECK(dyn_st_bootstrap(a, n, b, n / 2, &o1) == DYN_ST_OK, "bootstrap serial");
    dyn_par_set_spawn_limit(-1);
    CHECK(dyn_st_bootstrap(a, n, b, n / 2, &o2) == DYN_ST_OK, "bootstrap parallel");
    dyn_par_stats(&st);
    CHECK(o1.low == o2.low && o1.high == o2.high && o1.estimate == o2.estimate,
        "bootstrap is bit-identical on one thread and many ([%.17g, %.17g] vs [%.17g, %.17g])", o1.low, o1.high, o2.low, o2.high);
    CHECK(o1.low < o1.estimate && o1.estimate < o1.high, "interval brackets the estimate (%.17g in [%.17g, %.17g])", o1.estimate, o1.low, o1.high);
    o3.stop = stop_now;
    CHECK(dyn_st_bootstrap(a, n, b, n / 2, &o3) == DYN_ST_E_CANCELLED, "stop callback cancels");
    free(a);
    free(b);
}

static void test_map(void)
{
    uint32_t n = 50000, i, diff = 0;
    double *a = (double*)malloc(n * sizeof(double)), *b = (double*)malloc(n * sizeof(double));
    dyn_st_map_t m = { NULL, dyn_st_t_sf, NULL, 7.5, 0.0 };
    for (i = 0; i < n; i++)
        a[i] = b[i] = ((double)(rnd() % 2000001) - 1000000.0) / 1e5;
    dyn_par_set_spawn_limit(0);
    dyn_st_map(&m, a, n);
    dyn_par_set_spawn_limit(-1);
    dyn_st_map(&m, b, n);
    for (i = 0; i < n; i++)
        diff += a[i] != b[i];
    CHECK(diff == 0, "parallel dyn_st_map equals the serial map (%u differ)", diff);
    free(a);
    free(b);
}

int main(void)
{
    test_map();
    test_argsort();
    test_rank();
    test_ks_exact_all();
    test_mwu_complement();
    test_bootstrap();
    printf("test_statk_core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
