/*
 * test_dfk_core.c -- src/core/dfk (the dyna:dataframe grouped-aggregate
 * kernels) as a standalone C library.
 *
 * Oracle: a naive per-group reference written here from the d.ts contract
 * of GROUP_BY_SUM/MEAN/COUNT/MIN/MAX: sum and mean propagate NaN, count
 * counts rows, min/max ignore NaN values, an empty group is sum 0, count 0
 * and NaN for mean/min/max, and a group whose values are all NaN has NaN
 * min/max. Checked on hand cases and on 300 seeded
 * random tables over every key and value type, with and without a mask,
 * for every one of the 31 aggregate sets: the one-pass scan must give each
 * aggregate bit-for-bit what a scan for that aggregate alone gives.
 *
 * dfk_moments / dfk_widen (moved from dataframe-stats.c, behind describe,
 * std, var, skew, kurt, corr, cov and the regression family): oracle is a
 * two-pass central-moment sum shifted by the first selected value, so a
 * mean that is not representable (offsets near 1e15) cannot bias it the
 * way an unshifted two-pass would. 600 seeded columns over every type,
 * lengths around the 64-row unrolled cut, single and paired, masked and
 * not: n, lo and hi exact, sums within n*eps, centred moments within
 * 1e-11 of n*max|d|^k, m3/m4 NaN for pairs, widen equals df_get per row.
 *
 * dfo_* sort kernels (moved from dataframe-sort.c): oracle is a bottom-up
 * merge sort on the contract order (NaN last in both directions, ascending
 * or descending key, -0 == +0, ties by row index) over 420 seeded arrays
 * with NaN, +-0, +-inf and heavy duplicates, sizes across the comparison,
 * radix (256) and parallel radix (140k rows) cuts. sort_any/top/select get
 * rows in ascending index order, as dfo_gather produces them, because the
 * radix paths are stable rather than index-ordered; dfo_sort itself is
 * checked on shuffled indices with random depth budgets (qsort fallback).
 * dfo_f64_sort against the same order with -0 before +0 at EVERY size (shell sort
 * of bit keys below 64 values, radix from 64; D9: the old -1 below 64 sent callers to an
 * unstable qsort), dfo_pair_sort and dfo_pair_small_sort (shell sort of
 * (key, row) below 64, insertion above) as a stable sort of 16-byte records
 * (-0 == +0 keeps row order), dfo_gather against df_get.
 *
 * dfk_roll_extreme / dfk_roll_sum_sel (ROLLING_MIN/MAX/SUM/MEAN, moved
 * from dataframe-group-agg.c): oracle recomputes every window naively.
 * Values are multiples of 1/8 so every window sum is exact and the
 * unrolled, sliding-block and masked paths must match bit for bit; NaN
 * in float columns, windows on both sides of the 8 and 256 cuts, spans
 * shorter than the window (declined, rc -1), masked mean over selected
 * rows only (NaN when none), min/max skip NaN and unselected rows.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-dfk.h"
#include "../lib/dyn-xalloc.h"

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

static int same(double a, double b)
{
    return (isnan(a) && isnan(b)) || (a == b && signbit(a) == signbit(b));
}

static const unsigned BITS[5] = { DFK_AGG_SUM, DFK_AGG_MEAN, DFK_AGG_COUNT, DFK_AGG_MIN, DFK_AGG_MAX };

static void run(DFType kt, const void* kp, DFType vt, const void* vp, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, unsigned aggs, double out[5][64])
{
    double sum[64], mn[64], mx[64];
    uint32_t cnt[64];
    dfk_acc_t a = { sum, mn, mx, cnt };
    int k;
    dfk_group_scan(kt, kp, vt, vp, n, mask, ngroups, aggs, &a);
    for (k = 0; k < 5; k++)
        if (aggs & BITS[k])
            dfk_group_finish(BITS[k], &a, ngroups, out[k]);
}

static void reference(DFType kt, const void* kp, DFType vt, const void* vp, uint32_t n, const uint8_t* mask,
    uint32_t ngroups, double out[5][64])
{
    uint32_t g, i;
    for (g = 0; g < ngroups; g++) {
        double s = 0, lo = INFINITY, hi = -INFINITY;
        uint32_t c = 0, cv = 0;
        for (i = 0; i < n; i++) {
            double v;
            if ((mask && !mask[i]) || (uint32_t)df_get(kp, kt, i) != g)
                continue;
            v = df_get(vp, vt, i);
            s += v;
            if (v == v)
                cv++;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
            c++;
        }
        out[0][g] = s;
        out[1][g] = c ? s / c : NAN;
        out[2][g] = c;
        out[3][g] = cv ? lo : NAN;
        out[4][g] = cv ? hi : NAN;
    }
}

static void test_hand(void)
{
    static const int32_t k[] = { 0, 0, 1, 3, 3, 4 };
    static const double v[] = { 1, NAN, NAN, 2, 5, -0.0 };
    static const uint8_t mask[] = { 1, 0, 1, 1, 0, 1 };
    double out[5][64];
    uint32_t nk = 0, ng = 0;
    run(DF_I32, k, DF_F64, v, 6, NULL, 5, 31, out);
    CHECK(isnan(out[0][0]) && isnan(out[1][0]) && out[2][0] == 2 && out[3][0] == 1 && out[4][0] == 1, "NaN propagates into sum/mean, is counted, is skipped by min/max");
    CHECK(isnan(out[3][1]) && isnan(out[4][1]), "an all-NaN group has NaN min and max (d.ts: NaN is ignored, a group with no value is NaN)");
    CHECK(out[0][2] == 0 && isnan(out[1][2]) && out[2][2] == 0 && isnan(out[3][2]) && isnan(out[4][2]), "an empty group: sum 0, mean NaN, count 0, min/max NaN");
    CHECK(out[0][3] == 7 && out[1][3] == 3.5 && out[2][3] == 2 && out[3][3] == 2 && out[4][3] == 5, "group 3");
    CHECK(same(out[3][4], -0.0) && same(out[4][4], -0.0) && same(out[0][4], 0.0), "-0 survives min/max; 0 + -0 sums to +0");
    run(DF_I32, k, DF_F64, v, 6, mask, 5, 31, out);
    CHECK(out[2][0] == 1 && out[0][0] == 1 && out[2][3] == 1 && out[1][3] == 2, "the mask drops rows");
    CHECK(dfk_group_range(DF_I32, k, 6, &nk, &ng) == DFK_OK && nk == 5 && ng == 5, "range of 0..4 is 5 groups");
    CHECK(dfk_group_range(DF_F64, v, 6, &nk, &ng) == DFK_E_FLOAT_KEY, "a float key is refused");
    {
        static const int8_t neg[] = { 1, -1 };
        static const uint32_t big[] = { 0, DF_MAX_GROUPS };
        static const uint32_t edge[] = { DF_MAX_GROUPS - 1 };
        CHECK(dfk_group_range(DF_I8, neg, 2, &nk, &ng) == DFK_E_NEG_KEY, "a negative key is refused");
        CHECK(dfk_group_range(DF_U32, big, 2, &nk, &ng) == DFK_E_TOO_MANY, "key %d is one past the group limit", DF_MAX_GROUPS);
        CHECK(dfk_group_range(DF_U32, edge, 1, &nk, &ng) == DFK_OK && nk == DF_MAX_GROUPS, "key %d is the last allowed", DF_MAX_GROUPS - 1);
        CHECK(dfk_group_range(DF_U8, k, 0, &nk, &ng) == DFK_OK && nk == 0 && ng == 1, "no rows: 0 keys, 1 scratch group");
    }
    {
        static const double z[] = { -0.0, 0.0, 0.0, -0.0 };
        static const int32_t zk[] = { 0, 0, 1, 1 };
        run(DF_I32, zk, DF_F64, z, 4, NULL, 2, DFK_AGG_MIN | DFK_AGG_MAX, out);
        CHECK(same(out[3][0], -0.0) && same(out[4][0], -0.0) && same(out[3][1], 0.0) && same(out[4][1], 0.0), "min/max keep the first of equal values (-0 vs +0)");
    }
    {
        static const int32_t far[] = { 0, 9, 1 };
        static const double fv[] = { 1, 100, 2 };
        double* sum = malloc(2 * sizeof(double));
        double* mn = malloc(2 * sizeof(double));
        double* mx = malloc(2 * sizeof(double));
        uint32_t* cnt = xmalloc(2 * sizeof(uint32_t));
        dfk_acc_t a = { sum, mn, mx, cnt };
        dfk_group_scan(DF_I32, far, DF_F64, fv, 3, NULL, 2, 31, &a);
        CHECK(sum[0] == 1 && sum[1] == 2 && cnt[1] == 1, "a key past ngroups is ignored, not written (exact-size heap accumulators)");
        free(sum);
        free(mn);
        free(mx);
        free(cnt);
    }
    {
        static const uint8_t ku[] = { 0, 0, 1 };
        static const int8_t vi8[] = { -5, 3, -128 };
        static const int16_t vi16[] = { -300, 7, 2 };
        run(DF_U8, ku, DF_I8, vi8, 3, NULL, 2, DFK_AGG_SUM | DFK_AGG_MIN, out);
        CHECK(out[0][0] == -2 && out[3][0] == -5 && out[0][1] == -128, "int8 values are signed");
        run(DF_U8, ku, DF_I16, vi16, 3, NULL, 2, DFK_AGG_SUM, out);
        CHECK(out[0][0] == -293 && out[0][1] == 2, "int16 values are signed");
    }
    {
        static const int32_t codes[] = { 2, 0, 2, 7 };
        uint8_t seen[5];
        CHECK(dfk_keys_present(codes, 4, 5, seen) == 2 && seen[0] && !seen[1] && seen[2] && !seen[3] && !seen[4], "present keys are 0 and 2 (7 is out of range)");
    }
}

static uint32_t rs = 777;
static uint32_t rnd(uint32_t n)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) % n;
}

static void fill(DFType t, void* p, uint32_t i, double x)
{
    switch ((int)t) {
    case DF_F64:
        ((double*)p)[i] = x;
        break;
    case DF_F32:
        ((float*)p)[i] = (float)x;
        break;
    case DF_I32:
        ((int32_t*)p)[i] = (int32_t)(int64_t)x;
        break;
    case DF_U32:
        ((uint32_t*)p)[i] = (uint32_t)(int64_t)x;
        break;
    case DF_I16:
        ((int16_t*)p)[i] = (int16_t)(int64_t)x;
        break;
    case DF_U16:
        ((uint16_t*)p)[i] = (uint16_t)(int64_t)x;
        break;
    case DF_I8:
        ((int8_t*)p)[i] = (int8_t)(int64_t)x;
        break;
    default:
        ((uint8_t*)p)[i] = (uint8_t)(int64_t)x;
        break;
    }
}

static void test_random(void)
{
    static const DFType KT[] = { DF_I32, DF_U32, DF_I16, DF_U16, DF_I8, DF_U8 };
    static const DFType VT[] = { DF_F64, DF_F32, DF_I32, DF_I16, DF_U8, DF_I8, DF_U16, DF_U32 };
    int t, bad_ref = 0, bad_one = 0;
    for (t = 0; t < 300; t++) {
        DFType kt = KT[rnd(6)], vt = VT[rnd(8)];
        uint32_t n = rnd(200), ngroups = 1 + rnd(60), i, nk = 0, ng = 0;
        double kbuf[200], vbuf[200];
        uint8_t mbuf[200];
        const uint8_t* mask = rnd(2) ? mbuf : NULL;
        double ref[5][64], all[5][64], one[5][64];
        unsigned aggs;
        int k;
        for (i = 0; i < n; i++) {
            int isf = vt == DF_F64 || vt == DF_F32;
            fill(kt, kbuf, i, rnd(ngroups));
            fill(vt, vbuf, i, isf && rnd(8) == 0 ? NAN : isf ? (double)rnd(4000) / 10 - 50
                                                             : (double)rnd(100) - 50);
            mbuf[i] = (uint8_t)rnd(2);
        }
        if (dfk_group_range(kt, kbuf, n, &nk, &ng) != DFK_OK || nk > ngroups) {
            bad_ref++;
            continue;
        }
        reference(kt, kbuf, vt, vbuf, n, mask, ng, ref);
        run(kt, kbuf, vt, vbuf, n, mask, ng, 31, all);
        for (k = 0; k < 5; k++)
            for (i = 0; i < nk; i++)
                if (!same(ref[k][i], all[k][i]))
                    bad_ref++;
        for (aggs = 1; aggs < 32; aggs++) {
            run(kt, kbuf, vt, vbuf, n, mask, ng, aggs, all);
            for (k = 0; k < 5; k++) {
                if (!(aggs & BITS[k]))
                    continue;
                run(kt, kbuf, vt, vbuf, n, mask, ng, BITS[k], one);
                for (i = 0; i < nk; i++)
                    if (!same(one[k][i], all[k][i]))
                        bad_one++;
            }
        }
    }
    CHECK(bad_ref == 0, "the one-pass scan matches the naive reference on 300 random tables (%d bad)", bad_ref);
    CHECK(bad_one == 0, "every aggregate set gives each aggregate exactly what a scan for it alone gives (%d bad)", bad_one);
}

static int csv_run(const char* text, const DfkCsvSpec* in, DfkCsvOut* out)
{
    DfkCsvSpec s;
    if (in)
        s = *in;
    else
        memset(&s, 0, sizeof(s));
    if (!s.delim)
        s.delim = ',';
    if (!s.quote)
        s.quote = '"';
    if (!in)
        s.limit = UINT64_MAX;
    s.seed = 0x1234;
    return dfk_csv_read((const uint8_t*)text, strlen(text), &s, out);
}

static const char* csv_str(const DfkCsvOut* o, uint32_t c, uint32_t r)
{
    return o->cols[c].dict[o->cols[c].codes[r]];
}

static void test_csv(void)
{
    DfkCsvOut o;
    DfkCsvSpec s;
    int rc;

    rc = csv_run("id,name,x\n1,a,2.5\n2,b,\n3,a,7\n", NULL, &o);
    CHECK(rc == 0 && o.ncols == 3 && o.nrows == 3, "csv: 3 columns x 3 rows (rc %d)", rc);
    if (rc == 0) {
        CHECK(o.cols[0].kind == DFK_CSV_F64 && o.cols[1].kind == DFK_CSV_STR && o.cols[2].kind == DFK_CSV_F64, "csv: kinds f64 str f64");
        CHECK(o.cols[1].dict_len == 2 && strcmp(csv_str(&o, 1, 0), "a") == 0 && strcmp(csv_str(&o, 1, 1), "b") == 0 && o.cols[1].codes[2] == 0, "csv: dictionary codes in first-appearance order");
        CHECK(o.cols[2].f64[0] == 2.5 && isnan(o.cols[2].f64[1]) && o.cols[2].f64[2] == 7.0, "csv: empty f64 cell is NaN");
        CHECK(strcmp(o.cols[1].name, "name") == 0, "csv: column names from the header");
        dfk_csv_free(&o);
    }

    rc = csv_run("a\n1\n2\nz\n1\n", NULL, &o);
    CHECK(rc == 0 && o.cols[0].kind == DFK_CSV_STR && o.cols[0].dict_len == 3, "csv: a late non-number makes the column str (rc %d)", rc);
    if (rc == 0) {
        CHECK(strcmp(csv_str(&o, 0, 0), "1") == 0 && strcmp(csv_str(&o, 0, 2), "z") == 0 && o.cols[0].codes[3] == 0, "csv: the second pass keeps first-appearance order");
        dfk_csv_free(&o);
    }

    memset(&s, 0, sizeof(s));
    s.limit = UINT64_MAX;
    s.strict = 1;
    rc = csv_run("a,b\n1\n", &s, &o);
    CHECK(rc == DFK_CSV_E_WIDTH && o.err_record == 2, "csv: strict width error on record 2 (rc %d rec %llu)", rc, (unsigned long long)o.err_record);
    rc = csv_run("a\n\"x\n", &s, &o);
    CHECK(rc == DFK_CSV_E_QUOTE, "csv: strict unterminated quote (rc %d)", rc);
    rc = csv_run("a\n\"x\"y\n", &s, &o);
    CHECK(rc == DFK_CSV_E_GARBAGE, "csv: strict garbage after a quote (rc %d)", rc);
    rc = csv_run("", NULL, &o);
    CHECK(rc == DFK_CSV_E_EMPTY, "csv: empty input (rc %d)", rc);
    rc = csv_run("a,a\n1,2\n", NULL, &o);
    CHECK(rc == DFK_CSV_E_DUP, "csv: duplicate header (rc %d)", rc);

    {
        static const char* const want[] = { "c", "a" };
        static const char* const dn[] = { "a" };
        static const DfkCsvKind dk[] = { DFK_CSV_I32 };
        memset(&s, 0, sizeof(s));
        s.columns = want;
        s.ncolumns = 2;
        s.dtype_names = dn;
        s.dtype_kinds = dk;
        s.ndtypes = 1;
        s.offset = 1;
        s.limit = 1;
        rc = csv_run("a,b,c\n1,2,3\n4,5,6\n7,8,9\n", &s, &o);
        CHECK(rc == 0 && o.nrows == 1 && o.cols[0].f64[0] == 6.0 && o.cols[1].kind == DFK_CSV_I32 && o.cols[1].i32[0] == 4, "csv: columns, dtypes, offset and limit (rc %d)", rc);
        if (rc == 0)
            dfk_csv_free(&o);
        s.offset = 0;
        s.limit = UINT64_MAX;
        rc = csv_run("a,b,c\nx,2,3\n", &s, &o);
        CHECK(rc == DFK_CSV_E_INT && o.err_record == 2 && strstr(o.err_text, "'x'") != NULL, "csv: a forced i32 names the cell (rc %d: %s)", rc, o.err_text);
    }

    {
        static const char* const nums[] = { "0.1", "1e23", "4.9e-324", "9007199254740993", "-0", ".5", "5.",
            "123456789012345678901234567890", "2.2250738585072014e-308", "1.7976931348623157e308", "0.000001234", "1e-7" };
        char text[1024] = "v\n";
        size_t i;
        for (i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
            strcat(text, nums[i]);
            strcat(text, "\n");
        }
        rc = csv_run(text, NULL, &o);
        CHECK(rc == 0 && o.cols[0].kind == DFK_CSV_F64, "csv: every decimal form is f64 (rc %d)", rc);
        if (rc == 0) {
            for (i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
                double w = strtod(nums[i], NULL);
                CHECK(memcmp(&o.cols[0].f64[i], &w, sizeof(w)) == 0, "csv: '%s' parses bit-exactly as strtod (%.17g vs %.17g)", nums[i], o.cols[0].f64[i], w);
            }
            dfk_csv_free(&o);
        }
    }
}

static int near(double got, long double want, long double scale)
{
    long double d = (long double)got - want;
    if (d < 0)
        d = -d;
    return d <= 1e-11L * scale;
}

static void moments_ref(const double* x, const double* y, const uint8_t* m, uint32_t n, long double* r)
{
    long double c = 0, px = 0, py = 0, sx = 0, sy = 0, mx, my, a2 = 0, a3 = 0, a4 = 0, b2 = 0, cxy = 0;
    uint32_t i;
    int have = 0;
    for (i = 0; i < n; i++) {
        if (m && !m[i])
            continue;
        if (!have) {
            px = x[i];
            py = y ? y[i] : 0;
            have = 1;
        }
        c += 1;
        sx += x[i] - px;
        if (y)
            sy += y[i] - py;
    }
    mx = c > 0 ? sx / c : 0;
    my = c > 0 ? sy / c : 0;
    for (i = 0; i < n; i++) {
        long double d, e;
        if (m && !m[i])
            continue;
        d = (x[i] - px) - mx;
        a2 += d * d;
        a3 += d * d * d;
        a4 += d * d * d * d;
        if (y) {
            e = (y[i] - py) - my;
            b2 += e * e;
            cxy += d * e;
        }
    }
    r[0] = c;
    r[1] = px + mx;
    r[2] = py + my;
    r[3] = a2;
    r[4] = a3;
    r[5] = a4;
    r[6] = b2;
    r[7] = cxy;
}

static void test_moments(void)
{
    static const DFType TS[8] = { DF_F64, DF_F32, DF_I32, DF_U32, DF_I16, DF_U16, DF_I8, DF_U8 };
    static const uint32_t NS[9] = { 0, 1, 3, 7, 63, 64, 65, 200, 1031 };
    int it;
    {
        DfkMoments o;
        dfk_moments(NULL, DF_F64, NULL, DF_F64, NULL, 0, &o);
        CHECK(o.n == 0 && o.mx == 0 && o.m2x == 0 && o.lo == INFINITY && o.hi == -INFINITY, "moments: empty column");
    }
    for (it = 0; it < 600; it++) {
        DFType tx = TS[rnd(8)], ty = TS[rnd(8)];
        uint32_t n = NS[rnd(9)], i;
        int pair = (int)rnd(2), masked = (int)rnd(3) == 0;
        double off = rnd(2) ? 0.0 : (double)rnd(100);
        if (tx == DF_F64 && rnd(3) == 0)
            off = rnd(2) ? 1e6 + (double)rnd(1000) : 1e15 + (double)rnd(1000);
        void *px = calloc(n + 1, 8), *py = calloc(n + 1, 8);
        double *wx = calloc(n + 1, sizeof(double)), *wy = calloc(n + 1, sizeof(double));
        uint8_t* mk = calloc(n + 1, 1);
        long double r[8], sx = 0, sy = 0, smax = 1, tmax = 1, lo = INFINITY, hi = -INFINITY;
        DfkMoments o;
        for (i = 0; i < n; i++) {
            double v = off + (double)rnd(1000) / 8.0 - (tx == DF_U8 || tx == DF_U16 || tx == DF_U32 ? 0.0 : 40.0);
            double w = (double)rnd(1000) / 16.0 + 0.25 * v;
            fill(tx, px, i, v);
            fill(ty, py, i, w);
            mk[i] = (uint8_t)(rnd(4) != 0);
        }
        dfk_widen(px, tx, wx, n);
        dfk_widen(py, ty, wy, n);
        for (i = 0; i < n; i++)
            CHECK(same(wx[i], df_get(px, tx, i)), "widen: type %d row %u", (int)tx, i);
        moments_ref(wx, pair ? wy : NULL, masked ? mk : NULL, n, r);
        dfk_moments(px, tx, pair ? py : NULL, ty, masked ? mk : NULL, n, &o);
        for (i = 0; i < n; i++) {
            if (masked && !mk[i])
                continue;
            if (wx[i] < lo)
                lo = wx[i];
            if (wx[i] > hi)
                hi = wx[i];
            sx += wx[i];
            sy += wy[i];
            if (fabsl(wx[i] - r[1]) > smax)
                smax = fabsl(wx[i] - r[1]);
            if (fabsl(wy[i] - r[2]) > tmax)
                tmax = fabsl(wy[i] - r[2]);
        }
        CHECK(o.n == (double)r[0], "moments it %d: n %g vs %Lg", it, o.n, r[0]);
        CHECK(r[0] == 0 || ((long double)o.lo == lo && (long double)o.hi == hi), "moments it %d: lo/hi %g %g vs %Lg %Lg", it, o.lo, o.hi, lo, hi);
        CHECK(near(o.sx, sx, (r[0] + 1) * (fabsl(sx) + 1) * 1e-5L), "moments it %d: sx %.17g vs %.20Lg", it, o.sx, sx);
        CHECK(near(o.mx, r[1], 1e-4L * fabsl(r[1]) + smax), "moments it %d: mx %.17g vs %.20Lg", it, o.mx, r[1]);
        CHECK(near(o.m2x, r[3], (r[0] + 1) * smax * smax), "moments it %d n %u pair %d mask %d: m2x %.17g vs %.20Lg", it, n, pair, masked, o.m2x, r[3]);
        CHECK(o.m2x >= 0, "moments it %d: m2x negative", it);
        if (pair) {
            CHECK(near(o.sy, sy, (r[0] + 1) * (fabsl(sy) + 1) * 1e-5L), "moments it %d: sy %.17g vs %.20Lg", it, o.sy, sy);
            CHECK(near(o.my, r[2], 1e-4L * fabsl(r[2]) + tmax), "moments it %d: my %.17g vs %.20Lg", it, o.my, r[2]);
            CHECK(near(o.m2y, r[6], (r[0] + 1) * tmax * tmax), "moments it %d: m2y %.17g vs %.20Lg", it, o.m2y, r[6]);
            CHECK(near(o.cxy, r[7], (r[0] + 1) * smax * tmax), "moments it %d: cxy %.17g vs %.20Lg", it, o.cxy, r[7]);
            CHECK(isnan(o.m3x) && isnan(o.m4x), "moments it %d: pair m3x/m4x not NaN", it);
        } else {
            CHECK(o.sy == 0 && o.my == 0 && o.m2y == 0 && o.cxy == 0, "moments it %d: single leaves y zero", it);
            CHECK(near(o.m3x, r[4], (r[0] + 1) * smax * smax * smax), "moments it %d n %u mask %d: m3x %.17g vs %.20Lg", it, n, masked, o.m3x, r[4]);
            CHECK(near(o.m4x, r[5], (r[0] + 1) * smax * smax * smax * smax), "moments it %d n %u mask %d: m4x %.17g vs %.20Lg", it, n, masked, o.m4x, r[5]);
        }
        free(px);
        free(py);
        free(wx);
        free(wy);
        free(mk);
    }
}

static int ref_before(const DfoItem* a, const DfoItem* b, int desc)
{
    int na = isnan(a->key), nb = isnan(b->key);
    if (na || nb) {
        if (!na || !nb)
            return !na;
        return a->idx < b->idx;
    }
    if (a->key < b->key)
        return !desc;
    if (a->key > b->key)
        return desc;
    return a->idx < b->idx;
}

static void ref_sort(DfoItem* it, uint32_t n, int desc)
{
    uint32_t w, i, j;
    DfoItem* t = xmalloc((size_t)(n ? n : 1) * sizeof(*t));
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            uint32_t a = i, am = i + w < n ? i + w : n, b = am, bm = i + 2 * w < n ? i + 2 * w : n, k = i;
            while (a < am || b < bm)
                t[k++] = (b >= bm || (a < am && !ref_before(&it[b], &it[a], desc))) ? it[a++] : it[b++];
        }
        for (j = 0; j < n; j++)
            it[j] = t[j];
    }
    free(t);
}

static int same_items(const DfoItem* a, const DfoItem* b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i].idx != b[i].idx || !same(a[i].key, b[i].key))
            return 0;
    return 1;
}

static double sort_key(int pool)
{
    static const double SPECIAL[6] = { 0.0, -0.0, INFINITY, -INFINITY, 1e-300, -2.5 };
    uint32_t r = rnd(16);
    if (r == 0)
        return NAN;
    if (r == 1)
        return SPECIAL[rnd(6)];
    if (pool)
        return (double)rnd(9) - 4.0;
    return ((double)rnd(2000000) - 1000000.0) / 64.0;
}

static void test_sort(void)
{
    static const uint32_t NS[14] = { 0, 1, 2, 5, 24, 25, 63, 64, 255, 256, 300, 2047, 2048, 9000 };
    int it;
    for (it = 0; it < 420; it++) {
        uint32_t n = it >= 412 ? 140000u + rnd(1000) : NS[rnd(14)], i, k;
        int desc = (int)rnd(2), pool = (int)rnd(2);
        DfoItem *a = xmalloc((size_t)(n + 1) * sizeof(*a)), *b = malloc((size_t)(n + 1) * sizeof(*b)), *c = malloc((size_t)(n + 1) * sizeof(*c));
        for (i = 0; i < n; i++) {
            a[i].key = sort_key(pool);
            a[i].idx = i;
        }
        memcpy(b, a, (size_t)n * sizeof(*a));
        memcpy(c, a, (size_t)n * sizeof(*a));
        ref_sort(b, n, desc);
        dfo_sort_any(c, n, desc);
        CHECK(same_items(b, c, n), "sort_any it %d n %u desc %d pool %d", it, n, desc, pool);
        CHECK(dfo_valued(c, n) == dfo_valued(b, n), "valued it %d", it);
        for (i = 0, k = 0; i < n; i++)
            k += !isnan(a[i].key);
        CHECK(dfo_valued(c, n) == k, "valued it %d: %u vs %u", it, dfo_valued(c, n), k);
        if (n > 0 && n < 20000) {
            DfoItem* d = xmalloc((size_t)n * sizeof(*d));
            DfoItem* e = malloc((size_t)n * sizeof(*e));
            memcpy(d, a, (size_t)n * sizeof(*a));
            for (i = n; i > 1; i--) {
                uint32_t j = rnd(i);
                uint32_t t = d[i - 1].idx;
                d[i - 1].idx = d[j].idx;
                d[j].idx = t;
            }
            memcpy(e, d, (size_t)n * sizeof(*d));
            ref_sort(d, n, desc);
            dfo_sort(e, n, desc, 2 * (int)rnd(40));
            CHECK(same_items(d, e, n), "sort (shuffled idx) it %d n %u desc %d", it, n, desc);
            free(d);
            free(e);
            k = rnd(n) + 1;
            memcpy(c, a, (size_t)n * sizeof(*a));
            dfo_top(c, n, k, desc);
            CHECK(same_items(b, c, k), "top it %d n %u k %u desc %d", it, n, k, desc);
            k = rnd(n);
            memcpy(c, a, (size_t)n * sizeof(*a));
            dfo_select(c, n, k, desc);
            CHECK(same_items(b + k, c + k, 1), "select it %d n %u k %u desc %d", it, n, k, desc);
            for (i = 0; i < n; i++) {
                if (i < k)
                    CHECK(!ref_before(&c[k], &c[i], desc), "select it %d: row %u after pivot", it, i);
                else if (i > k)
                    CHECK(!ref_before(&c[i], &c[k], desc), "select it %d: row %u before pivot", it, i);
            }
        }
        {
            double* f = xmalloc((size_t)(n + 1) * sizeof(double));
            double* g = xmalloc((size_t)(n + 1) * sizeof(double));
            int rc;
            for (i = 0; i < n; i++)
                f[i] = g[i] = a[i].key;
            rc = dfo_f64_sort(f, n);
            CHECK(rc == 0, "f64_sort it %d rc %d", it, rc);
            {
                for (i = 0; i < n; i++)
                    b[i].key = g[i], b[i].idx = 0;
                ref_sort(b, n, 0);
                for (i = 0; i < n; i++) {
                    if (i + 1 < n && b[i].key == 0.0 && b[i + 1].key == 0.0 && !signbit(b[i].key) && signbit(b[i + 1].key)) {
                        double t = b[i].key;
                        b[i].key = b[i + 1].key;
                        b[i + 1].key = t;
                        i = i > 0 ? i - 2 : (uint32_t)-1;
                    }
                }
                for (i = 0, k = 1; i < n; i++)
                    k &= same(f[i], b[i].key);
                CHECK(k, "f64_sort it %d n %u", it, n);
            }
            free(f);
            free(g);
        }
        free(a);
        free(b);
        free(c);
    }
    {
        struct rec {
            double key;
            uint64_t tag;
        };
        for (it = 0; it < 120; it++) {
            uint32_t n = NS[rnd(14)], i, j;
            struct rec *r = xmalloc((size_t)(n + 1) * sizeof(*r)), *s = malloc((size_t)(n + 1) * sizeof(*s));
            int rc;
            for (i = 0; i < n; i++) {
                do
                    r[i].key = sort_key((int)rnd(2));
                while (isnan(r[i].key));
                r[i].tag = i;
            }
            memcpy(s, r, (size_t)n * sizeof(*r));
            {
                struct rec* u = malloc((size_t)(n + 1) * sizeof(*u));
                memcpy(u, r, (size_t)n * sizeof(*r));
                dfo_pair_small_sort(u, n);
                for (i = 1; i < n; i++) {
                    struct rec t = s[i];
                    for (j = i; j > 0 && s[j - 1].key > t.key; j--)
                        s[j] = s[j - 1];
                    s[j] = t;
                }
                CHECK(memcmp(u, s, (size_t)n * sizeof(*u)) == 0, "pair_small_sort it %d n %u: not the stable key order", it, n);
                memcpy(s, r, (size_t)n * sizeof(*r));
                free(u);
            }
            rc = dfo_pair_sort(r, n, 0);
            CHECK(rc == (n < DFO_SMALL_RADIX_MIN ? -1 : 0), "pair_sort it %d rc %d", it, rc);
            for (i = 1; i < n; i++) {
                struct rec t = s[i];
                for (j = i; j > 0 && s[j - 1].key > t.key; j--)
                    s[j] = s[j - 1];
                s[j] = t;
            }
            if (rc == 0)
                CHECK(memcmp(r, s, (size_t)n * sizeof(*r)) == 0, "pair_sort it %d n %u: not the stable key order", it, n);
            free(r);
            free(s);
        }
    }
    {
        static const DFType TS[8] = { DF_F64, DF_F32, DF_I32, DF_U32, DF_I16, DF_U16, DF_I8, DF_U8 };
        for (it = 0; it < 80; it++) {
            DFType t = TS[rnd(8)];
            uint32_t n = NS[rnd(10)], i, k = 0, got;
            void* p = calloc(n + 1, 8);
            uint8_t* m = xmalloc(n + 1);
            int masked = (int)rnd(2);
            DfoItem* g = malloc((size_t)(n + 1) * sizeof(*g));
            for (i = 0; i < n; i++) {
                fill(t, p, i, (double)rnd(200) - (t == DF_U8 || t == DF_U16 || t == DF_U32 ? 0.0 : 100.0));
                m[i] = (uint8_t)rnd(2);
            }
            got = dfo_gather(p, t, masked ? m : NULL, n, g);
            for (i = 0; i < n; i++) {
                if (masked && !m[i])
                    continue;
                CHECK(k < got && g[k].idx == i && same(g[k].key, df_get(p, t, i)), "gather it %d type %d row %u", it, (int)t, i);
                k++;
            }
            CHECK(got == k, "gather it %d: %u rows vs %u", it, got, k);
            free(p);
            free(m);
            free(g);
        }
    }
}

static void test_roll(void)
{
    static const DFType TS[8] = { DF_F64, DF_F32, DF_I32, DF_U32, DF_I16, DF_U16, DF_I8, DF_U8 };
    static const uint32_t WS[10] = { 1, 2, 7, 8, 9, 31, 255, 256, 257, 700 };
    int it;
    for (it = 0; it < 300; it++) {
        DFType t = TS[rnd(8)];
        uint32_t w = WS[rnd(10)], span = rnd(3) ? w + rnd(3 * w + 5) : rnd(w + 1), i, j;
        int masked = (int)rnd(2), mean = (int)rnd(2), want_min = (int)rnd(2), rc, nan_ok = t == DF_F64 || t == DF_F32;
        void* p = calloc(span + 1, 8);
        uint8_t* m = malloc(span + 1);
        double *x = malloc((span + 1) * sizeof(double)), *d = malloc((span + 1) * sizeof(double));
        for (i = 0; i < span; i++) {
            double v = (double)rnd(4000) / 8.0 - (t == DF_U8 || t == DF_U16 || t == DF_U32 ? 0.0 : 200.0);
            fill(t, p, i, nan_ok && rnd(40) == 0 ? NAN : v);
            m[i] = (uint8_t)(rnd(5) != 0);
        }
        dfk_widen(p, t, x, span);
        rc = dfk_roll_extreme(p, t, masked ? m : NULL, d, span, w, want_min);
        CHECK(rc == 0, "roll_extreme it %d rc %d", it, rc);
        for (i = 0; rc == 0 && i < span; i++) {
            double e = NAN;
            if (i + 1 >= w)
                for (j = i + 1 - w; j <= i; j++)
                    if ((!masked || m[j]) && !isnan(x[j]) && (isnan(e) || (want_min ? x[j] < e : x[j] > e)))
                        e = x[j];
            CHECK(same(d[i], e), "roll_extreme it %d w %u span %u mask %d min %d row %u: %g vs %g", it, w, span, masked, want_min, i, d[i], e);
        }
        rc = dfk_roll_sum_sel(p, t, masked ? m : NULL, d, span, w, mean);
        CHECK(rc == ((span < w || w < (masked ? DFK_ROLL_SLIDE_MIN : 8u)) ? -1 : 0), "roll_sum it %d w %u span %u mask %d rc %d", it, w, span, masked, rc);
        for (i = 0; rc == 0 && i < span; i++) {
            double s = 0.0, e = NAN;
            uint32_t c = 0;
            if (i + 1 >= w) {
                for (j = i + 1 - w; j <= i; j++)
                    if (!masked || m[j]) {
                        s += x[j];
                        c++;
                    }
                e = !mean ? s : masked ? (c ? s / (double)c : NAN) : s / (double)w;
            }
            CHECK(same(d[i], e), "roll_sum it %d w %u span %u mask %d mean %d row %u: %.17g vs %.17g", it, w, span, masked, mean, i, d[i], e);
        }
        free(p);
        free(m);
        free(x);
        free(d);
    }
}

int main(void)
{
    test_hand();
    test_random();
    test_csv();
    test_moments();
    test_sort();
    test_roll();
    printf("test_dfk_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
