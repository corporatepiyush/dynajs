#include "statk-int.h"

#include <stdlib.h>
#include <string.h>

static double st_sum(const double* x, uint32_t n)
{
    double s = 0.0, c = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double t = s + x[i];
        c += fabs(s) >= fabs(x[i]) ? (s - t) + x[i] : (x[i] - t) + s;
        s = t;
    }
    return s + c;
}

static void st_mean_var(const double* x, uint32_t n, double* mean, double* var)
{
    double m = st_sum(x, n) / (double)n, ss = 0.0, cs = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double d = x[i] - m;
        ss += d * d;
        cs += d;
    }
    *mean = m;
    *var = n > 1 ? (ss - cs * cs / (double)n) / (double)(n - 1) : DYN_NAN;
}

double dyn_st_p_from_t(double t, double df, dyn_st_alt_t alt)
{
    double p;
    if (isnan(t))
        return DYN_NAN;
    switch (alt) {
    case DYN_ST_LESS:
        return dyn_st_t_cdf(t, df);
    case DYN_ST_GREATER:
        return dyn_st_t_sf(t, df);
    case DYN_ST_TWO_SIDED:
    default:
        p = 2.0 * dyn_st_t_sf(fabs(t), df);
        return p > 1.0 ? 1.0 : p;
    }
}

double dyn_st_p_from_z(double z, dyn_st_alt_t alt)
{
    double p;
    if (isnan(z))
        return DYN_NAN;
    switch (alt) {
    case DYN_ST_LESS:
        return dyn_st_norm_cdf(z);
    case DYN_ST_GREATER:
        return dyn_st_norm_sf(z);
    case DYN_ST_TWO_SIDED:
    default:
        p = 2.0 * dyn_st_norm_sf(fabs(z));
        return p > 1.0 ? 1.0 : p;
    }
}

static void st_nan_result(dyn_st_result_t* r, double df)
{
    r->statistic = DYN_NAN;
    r->pvalue = DYN_NAN;
    r->df = df;
}

int dyn_st_ttest_1samp(const double* a, uint32_t n, double mu, dyn_st_alt_t alt, dyn_st_result_t* r)
{
    double m, v;
    if (n < 2) {
        st_nan_result(r, n ? (double)n - 1.0 : DYN_NAN);
        return DYN_ST_OK;
    }
    if (dyn_st_has_nan(a, n))
        return DYN_ST_E_NAN;
    st_mean_var(a, n, &m, &v);
    r->df = (double)n - 1.0;
    r->statistic = (m - mu) / sqrt(v / (double)n);
    r->pvalue = dyn_st_p_from_t(r->statistic, r->df, alt);
    return DYN_ST_OK;
}

int dyn_st_ttest_ind(const double* a, uint32_t na, const double* b, uint32_t nb, int equal_var, dyn_st_alt_t alt,
    dyn_st_result_t* r)
{
    double ma, va, mb, vb, se2;
    if (na < 2 || nb < 2) {
        st_nan_result(r, DYN_NAN);
        return DYN_ST_OK;
    }
    if (dyn_st_has_nan(a, na) || dyn_st_has_nan(b, nb))
        return DYN_ST_E_NAN;
    st_mean_var(a, na, &ma, &va);
    st_mean_var(b, nb, &mb, &vb);
    if (equal_var) {
        double df = (double)na + (double)nb - 2.0;
        double sp2 = (((double)na - 1.0) * va + ((double)nb - 1.0) * vb) / df;
        se2 = sp2 * (1.0 / (double)na + 1.0 / (double)nb);
        r->df = df;
    } else {
        double sa = va / (double)na, sb = vb / (double)nb;
        se2 = sa + sb;
        r->df = se2 * se2 / (sa * sa / ((double)na - 1.0) + sb * sb / ((double)nb - 1.0));
    }
    r->statistic = (ma - mb) / sqrt(se2);
    r->pvalue = dyn_st_p_from_t(r->statistic, r->df, alt);
    return DYN_ST_OK;
}

int dyn_st_ttest_rel(const double* a, const double* b, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r)
{
    double* d;
    uint32_t i;
    int rc;
    if (n < 2) {
        st_nan_result(r, n ? (double)n - 1.0 : DYN_NAN);
        return DYN_ST_OK;
    }
    d = (double*)malloc((size_t)n * sizeof(double));
    if (!d)
        return DYN_ST_E_OOM;
    for (i = 0; i < n; i++)
        d[i] = a[i] - b[i];
    rc = dyn_st_ttest_1samp(d, n, 0.0, alt, r);
    free(d);
    return rc;
}

int dyn_st_ztest_prop(double c1, double n1, double c2, double n2, dyn_st_alt_t alt, dyn_st_result_t* r)
{
    double p1, p2, p;
    if (!(n1 > 0.0) || !(n2 > 0.0) || !(c1 >= 0.0) || !(c2 >= 0.0) || c1 > n1 || c2 > n2 || isinf(n1)
        || isinf(n2))
        return DYN_ST_E_DOMAIN;
    p1 = c1 / n1;
    p2 = c2 / n2;
    p = (c1 + c2) / (n1 + n2);
    r->df = DYN_NAN;
    r->statistic = (p1 - p2) / sqrt(p * (1.0 - p) * (1.0 / n1 + 1.0 / n2));
    r->pvalue = dyn_st_p_from_z(r->statistic, alt);
    return DYN_ST_OK;
}

static double st_mwu_sf_exact(uint32_t n1, uint32_t n2, uint64_t k, int* ok)
{
    uint64_t total = (uint64_t)n1 * n2, kc, m, u, d;
    double *freq, *sig, logtot, res;
    int upper;
    uint32_t i;
    if (n1 > n2) {
        uint32_t t = n1;
        n1 = n2;
        n2 = t;
    }
    *ok = 1;
    kc = total - k;
    upper = k < kc;
    m = upper ? k : kc;
    freq = (double*)malloc((size_t)(m + 1) * sizeof(double));
    sig = (double*)calloc((size_t)(m + 1), sizeof(double));
    if (!freq || !sig) {
        free(freq);
        free(sig);
        *ok = 0;
        return DYN_NAN;
    }
    for (d = 1; d <= n1; d++)
        for (u = d; u <= m; u += d)
            sig[u] += (double)d;
    for (d = (uint64_t)n2 + 1; d <= (uint64_t)n2 + n1; d++)
        for (u = d; u <= m; u += d)
            sig[u] -= (double)d;
    freq[0] = 1.0;
    for (u = 1; u <= m; u++) {
        double s = 0.0;
        for (d = 0; d < u; d++)
            s += freq[d] * sig[u - d];
        freq[u] = s / (double)u;
    }
    logtot = dyn_st_lgamma((double)n1 + (double)n2 + 1.0) - dyn_st_lgamma((double)n1 + 1.0) - dyn_st_lgamma((double)n2 + 1.0);
    {
        double cdf = 0.0, scale = exp(-logtot);
        for (i = 0; i <= m; i++)
            cdf += freq[i] * scale;
        res = upper ? 1.0 - cdf + freq[m] * scale : cdf;
    }
    free(freq);
    free(sig);
    return res;
}

#define ST_MWU_EXACT_AUTO_MAX (1u << 14)
#define ST_MWU_EXACT_MAX (1u << 17)

int dyn_st_mannwhitney(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_alt_t alt,
    dyn_st_method_t method, int continuity, dyn_st_result_t* r)
{
    uint32_t n, i;
    double *all, *rk, tie, r1 = 0.0, u1, u2, u;
    int rc;
    if (na == 0 || nb == 0)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(a, na) || dyn_st_has_nan(b, nb))
        return DYN_ST_E_NAN;
    n = na + nb;
    all = (double*)malloc((size_t)n * 2 * sizeof(double));
    if (!all)
        return DYN_ST_E_OOM;
    rk = all + n;
    memcpy(all, a, (size_t)na * sizeof(double));
    memcpy(all + na, b, (size_t)nb * sizeof(double));
    rc = dyn_st_rank(all, n, DYN_ST_RANK_AVERAGE, rk, &tie);
    if (rc != DYN_ST_OK) {
        free(all);
        return rc;
    }
    for (i = 0; i < na; i++)
        r1 += rk[i];
    free(all);
    u1 = r1 - (double)na * ((double)na + 1.0) / 2.0;
    u2 = (double)na * (double)nb - u1;
    u = alt == DYN_ST_GREATER ? u1 : (alt == DYN_ST_LESS ? u2 : (u1 > u2 ? u1 : u2));
    r->statistic = u1;
    r->df = DYN_NAN;
    if (method == DYN_ST_AUTO)
        method = ((na > 8 && nb > 8) || tie > 0.0 || (uint64_t)na * nb > ST_MWU_EXACT_AUTO_MAX) ? DYN_ST_ASYMP
                                                                                                : DYN_ST_EXACT;
    if (method == DYN_ST_EXACT) {
        int ok;
        double p;
        if ((uint64_t)na * nb > ST_MWU_EXACT_MAX)
            return DYN_ST_E_SIZE;
        p = st_mwu_sf_exact(na, nb, (uint64_t)u, &ok);
        if (!ok)
            return DYN_ST_E_OOM;
        p *= alt == DYN_ST_TWO_SIDED ? 2.0 : 1.0;
        r->pvalue = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    } else {
        double mu = (double)na * (double)nb / 2.0, nn = (double)n;
        double s = sqrt((double)na * (double)nb / 12.0 * ((nn + 1.0) - tie / (nn * (nn - 1.0))));
        double num = u1 - mu, z, p;
        if (continuity) {
            double sg = alt == DYN_ST_GREATER ? 1.0
                : alt == DYN_ST_LESS          ? -1.0
                                              : (num > 0.0 ? 1.0 : (num < 0.0 ? -1.0 : 0.0));
            num -= 0.5 * sg;
        }
        z = num / s;
        p = dyn_st_p_from_z(z, alt);
        r->pvalue = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    }
    return DYN_ST_OK;
}

static double st_ks_outside_square(uint32_t n, uint64_t h)
{
    double P = 0.0;
    int64_t k = (int64_t)(n / h);
    while (k >= 0) {
        double p1 = 1.0;
        uint64_t j;
        for (j = 0; j < h; j++)
            p1 = ((double)n - (double)k * (double)h - (double)j) * p1 / ((double)n + (double)k * (double)h + (double)j + 1.0);
        P = p1 * (1.0 - P);
        k--;
    }
    return 2.0 * P;
}

static double st_ks_outer_inside(uint32_t m, uint32_t n, uint64_t g, uint64_t h, int* ok)
{
    uint64_t mg, ng;
    int64_t minj, maxj, curlen, lenA, i;
    double* A;
    double res;
    *ok = 1;
    if (m < n) {
        uint32_t t = m;
        m = n;
        n = t;
    }
    mg = m / g;
    ng = n / g;
    minj = 0;
    maxj = (int64_t)((h + mg - 1) / mg);
    if (maxj > (int64_t)n + 1)
        maxj = (int64_t)n + 1;
    curlen = maxj - minj;
    lenA = 2 * maxj + 2;
    if (lenA > (int64_t)n + 1)
        lenA = (int64_t)n + 1;
    A = (double*)malloc((size_t)lenA * sizeof(double));
    if (!A) {
        *ok = 0;
        return DYN_NAN;
    }
    for (i = 0; i < lenA; i++)
        A[i] = i < maxj ? 0.0 : 1.0;
    for (i = 1; i <= (int64_t)m; i++) {
        int64_t lastminj = minj, lastlen = curlen, jj, lo, hi;
        double val;
        lo = (int64_t)ng * i - (int64_t)h;
        minj = (lo >= 0 ? lo / (int64_t)mg : -((-lo + (int64_t)mg - 1) / (int64_t)mg)) + 1;
        if (minj < 0)
            minj = 0;
        if (minj > (int64_t)n)
            minj = (int64_t)n;
        hi = (int64_t)ng * i + (int64_t)h;
        maxj = (hi + (int64_t)mg - 1) / (int64_t)mg;
        if (maxj > (int64_t)n + 1)
            maxj = (int64_t)n + 1;
        if (maxj <= minj) {
            free(A);
            return 1.0;
        }
        val = minj == 0 ? 0.0 : 1.0;
        for (jj = 0; jj < maxj - minj; jj++) {
            int64_t j = jj + minj;
            val = (A[jj + minj - lastminj] * (double)i + val * (double)j) / ((double)i + (double)j);
            A[jj] = val;
        }
        curlen = maxj - minj;
        if (lastlen > curlen)
            for (jj = maxj - minj; jj < maxj - minj + (lastlen - curlen) && jj < lenA; jj++)
                A[jj] = 1.0;
    }
    res = A[maxj - minj - 1];
    free(A);
    return res;
}

static double st_binom(double n, double k)
{
    double r = 1.0, i;
    if (k > n - k)
        k = n - k;
    for (i = 1.0; i <= k; i++)
        r = r * (n - k + i) / i;
    return r;
}

static double st_ks_paths_outside(uint32_t m, uint32_t n, uint64_t g, uint64_t h, int* ok)
{
    uint64_t mg, ng, lxj, j, i;
    double *B, num = 0.0;
    uint64_t* xj;
    *ok = 1;
    if (m < n) {
        uint32_t t = m;
        m = n;
        n = t;
    }
    mg = m / g;
    ng = n / g;
    {
        int64_t q = (int64_t)mg - (int64_t)h;
        int64_t fl = q >= 0 ? q / (int64_t)mg : -((-q + (int64_t)mg - 1) / (int64_t)mg);
        int64_t l = (int64_t)n + fl;
        lxj = l > 0 ? (uint64_t)l : 0;
    }
    if (lxj == 0)
        return st_binom((double)m + n, (double)n);
    B = (double*)malloc((size_t)lxj * sizeof(double));
    xj = (uint64_t*)malloc((size_t)lxj * sizeof(uint64_t));
    if (!B || !xj) {
        free(B);
        free(xj);
        *ok = 0;
        return DYN_NAN;
    }
    for (j = 0; j < lxj; j++)
        xj[j] = (h + mg * j + ng - 1) / ng;
    B[0] = 1.0;
    for (j = 1; j < lxj; j++) {
        double bj = st_binom((double)(xj[j] + j), (double)j);
        for (i = 0; i < j; i++)
            bj -= st_binom((double)(xj[j] - xj[i] + j - i), (double)(j - i)) * B[i];
        B[j] = bj;
    }
    for (j = 0; j < lxj; j++)
        if (xj[j] <= m)
            num += B[j] * st_binom((double)(m - xj[j]) + (double)(n - j), (double)(n - j));
    free(B);
    free(xj);
    return num;
}

static uint64_t st_gcd(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int st_ks_exact(uint32_t n1, uint32_t n2, double d, dyn_st_alt_t alt, double* prob)
{
    uint64_t g = st_gcd(n1, n2), lcm = ((uint64_t)n1 / g) * n2, h;
    double p;
    int ok = 1;
    if ((double)(n1 / g) >= 2147483647.0 / (double)(n2 / g))
        return 0;
    h = (uint64_t)llround(d * (double)lcm);
    if (h == 0) {
        *prob = 1.0;
        return 1;
    }
    if (alt == DYN_ST_TWO_SIDED) {
        p = n1 == n2 ? st_ks_outside_square(n1, h) : st_ks_outer_inside(n1, n2, g, h, &ok);
    } else if (n1 == n2) {
        uint64_t j;
        p = 1.0;
        for (j = 0; j < h; j++)
            p *= ((double)n1 - (double)j) / ((double)n1 + (double)j + 1.0);
    } else {
        double paths = st_ks_paths_outside(n1, n2, g, h, &ok), bin = st_binom((double)n1 + n2, (double)n1);
        if (!ok || paths > bin || isinf(bin))
            return 0;
        p = paths / bin;
    }
    if (!ok || !(p >= 0.0 && p <= 1.0))
        return 0;
    *prob = p;
    return 1;
}

#define ST_KS_AUTO_MAX 10000u

int dyn_st_ks2(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_alt_t alt, dyn_st_method_t method,
    dyn_st_result_t* r)
{
    double *x, *y, maxs = 0.0, mins = 0.0, d, prob = DYN_NAN;
    uint32_t i = 0, j = 0;
    int rc;
    if (na == 0 || nb == 0)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(a, na) || dyn_st_has_nan(b, nb))
        return DYN_ST_E_NAN;
    x = (double*)malloc(((size_t)na + nb) * sizeof(double));
    if (!x)
        return DYN_ST_E_OOM;
    y = x + na;
    rc = dyn_st_sort_copy(a, na, x);
    if (rc == DYN_ST_OK)
        rc = dyn_st_sort_copy(b, nb, y);
    if (rc != DYN_ST_OK) {
        free(x);
        return rc;
    }
    while (i < na || j < nb) {
        double v = j >= nb || (i < na && x[i] <= y[j]) ? x[i] : y[j], diff;
        while (i < na && x[i] <= v)
            i++;
        while (j < nb && y[j] <= v)
            j++;
        diff = (double)i / (double)na - (double)j / (double)nb;
        if (diff > maxs)
            maxs = diff;
        if (-diff > mins)
            mins = -diff;
    }
    free(x);
    if (mins > 1.0)
        mins = 1.0;
    d = alt == DYN_ST_LESS ? mins : (alt == DYN_ST_GREATER ? maxs : (mins > maxs ? mins : maxs));
    if (method == DYN_ST_AUTO)
        method = (na > nb ? na : nb) <= ST_KS_AUTO_MAX ? DYN_ST_EXACT : DYN_ST_ASYMP;
    if (method == DYN_ST_EXACT && !st_ks_exact(na, nb, d, alt, &prob))
        method = DYN_ST_ASYMP;
    if (method == DYN_ST_ASYMP) {
        double m = na > nb ? na : nb, n = na > nb ? nb : na, en = m * n / (m + n);
        if (alt == DYN_ST_TWO_SIDED) {
            prob = dyn_st_kolmogorov_sf(sqrt(en) * d);
        } else {
            double z = sqrt(en) * d;
            prob = exp(-2.0 * z * z - 2.0 * z * (m + 2.0 * n) / sqrt(m * n * (m + n)) / 3.0);
        }
    }
    r->statistic = d;
    r->df = DYN_NAN;
    r->pvalue = prob < 0.0 ? 0.0 : (prob > 1.0 ? 1.0 : prob);
    return DYN_ST_OK;
}

int dyn_st_chisquare(const double* obs, const double* exp_, uint32_t n, uint32_t ddof, dyn_st_result_t* r)
{
    double so = 0.0, se = 0.0, stat = 0.0, mean;
    uint32_t i;
    if (n == 0)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(obs, n) || (exp_ && dyn_st_has_nan(exp_, n)))
        return DYN_ST_E_NAN;
    so = st_sum(obs, n);
    if (exp_) {
        se = st_sum(exp_, n);
        if (fabs(so - se) > 1e-8 * (fabs(so) > fabs(se) ? fabs(so) : fabs(se)))
            return DYN_ST_E_DOMAIN;
    }
    mean = so / (double)n;
    for (i = 0; i < n; i++) {
        double e = exp_ ? exp_[i] : mean, d = obs[i] - e;
        stat += d * d / e;
    }
    r->statistic = stat;
    r->df = (double)n - 1.0 - (double)ddof;
    r->pvalue = r->df > 0.0 ? dyn_st_chi2_sf(stat, r->df) : DYN_NAN;
    return DYN_ST_OK;
}

int dyn_st_chi2_contingency(const double* t, uint32_t rows, uint32_t cols, int correction, double* expected,
    dyn_st_result_t* r)
{
    double *rs, *cs, total = 0.0, stat = 0.0, dof;
    uint32_t i, j;
    if (rows == 0 || cols == 0)
        return DYN_ST_E_SHAPE;
    if (dyn_st_has_nan(t, rows * cols))
        return DYN_ST_E_NAN;
    for (i = 0; i < rows * cols; i++)
        if (t[i] < 0.0)
            return DYN_ST_E_DOMAIN;
    rs = (double*)calloc((size_t)rows + cols, sizeof(double));
    if (!rs)
        return DYN_ST_E_OOM;
    cs = rs + rows;
    for (i = 0; i < rows; i++)
        for (j = 0; j < cols; j++) {
            rs[i] += t[i * cols + j];
            cs[j] += t[i * cols + j];
        }
    for (i = 0; i < rows; i++)
        total += rs[i];
    for (i = 0; i < rows; i++)
        for (j = 0; j < cols; j++) {
            double e = rs[i] * cs[j] / total;
            if (!(e > 0.0)) {
                free(rs);
                return DYN_ST_E_DOMAIN;
            }
            expected[i * cols + j] = e;
        }
    free(rs);
    dof = (double)rows * cols - (double)rows - (double)cols + 1.0;
    r->df = dof;
    if (dof == 0.0) {
        r->statistic = 0.0;
        r->pvalue = 1.0;
        return DYN_ST_OK;
    }
    for (i = 0; i < rows * cols; i++) {
        double o = t[i], e = expected[i], d;
        if (correction && dof == 1.0) {
            double diff = e - o, mag = fabs(diff) < 0.5 ? fabs(diff) : 0.5;
            o += diff > 0.0 ? mag : (diff < 0.0 ? -mag : 0.0);
        }
        d = o - e;
        stat += d * d / e;
    }
    r->statistic = stat;
    r->pvalue = dyn_st_chi2_sf(stat, dof);
    return DYN_ST_OK;
}

static double st_corr(const double* x, const double* y, uint32_t n)
{
    double mx = st_sum(x, n) / (double)n, my = st_sum(y, n) / (double)n, sxx = 0.0, syy = 0.0, sxy = 0.0, rr;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double dx = x[i] - mx, dy = y[i] - my;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    if (sxx == 0.0 || syy == 0.0)
        return DYN_NAN;
    rr = sxy / (sqrt(sxx) * sqrt(syy));
    return rr > 1.0 ? 1.0 : (rr < -1.0 ? -1.0 : rr);
}

int dyn_st_pearson(const double* x, const double* y, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r)
{
    double rr, ab;
    if (n < 2)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(x, n) || dyn_st_has_nan(y, n))
        return DYN_ST_E_NAN;
    rr = st_corr(x, y, n);
    r->statistic = rr;
    r->df = (double)n - 2.0;
    if (isnan(rr)) {
        r->pvalue = DYN_NAN;
        return DYN_ST_OK;
    }
    if (n == 2) {
        r->pvalue = alt == DYN_ST_TWO_SIDED ? 1.0 : 0.5;
        return DYN_ST_OK;
    }
    ab = (double)n / 2.0 - 1.0;
    switch (alt) {
    case DYN_ST_LESS:
        r->pvalue = dyn_betainc((1.0 + rr) / 2.0, ab, ab);
        break;
    case DYN_ST_GREATER:
        r->pvalue = dyn_betainc((1.0 - rr) / 2.0, ab, ab);
        break;
    case DYN_ST_TWO_SIDED:
    default:
        r->pvalue = 2.0 * dyn_betainc((1.0 - fabs(rr)) / 2.0, ab, ab);
        if (r->pvalue > 1.0)
            r->pvalue = 1.0;
        break;
    }
    return DYN_ST_OK;
}

int dyn_st_spearman(const double* x, const double* y, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r)
{
    double *rx, rr, dof, t;
    int rc;
    if (n < 2)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(x, n) || dyn_st_has_nan(y, n))
        return DYN_ST_E_NAN;
    rx = (double*)malloc((size_t)n * 2 * sizeof(double));
    if (!rx)
        return DYN_ST_E_OOM;
    rc = dyn_st_rank(x, n, DYN_ST_RANK_AVERAGE, rx, NULL);
    if (rc == DYN_ST_OK)
        rc = dyn_st_rank(y, n, DYN_ST_RANK_AVERAGE, rx + n, NULL);
    if (rc != DYN_ST_OK) {
        free(rx);
        return rc;
    }
    rr = st_corr(rx, rx + n, n);
    free(rx);
    dof = (double)n - 2.0;
    r->statistic = rr;
    r->df = dof;
    if (isnan(rr)) {
        r->pvalue = DYN_NAN;
        return DYN_ST_OK;
    }
    t = rr * sqrt(dof / ((rr + 1.0) * (1.0 - rr)));
    r->pvalue = dof > 0.0 ? dyn_st_p_from_t(t, dof, alt) : DYN_NAN;
    return DYN_ST_OK;
}

int dyn_st_p_adjust(const double* p, uint32_t n, dyn_st_adjust_t method, double* out)
{
    uint32_t *idx, k;
    double run, by = 1.0;
    int rc;
    if (n == 0)
        return DYN_ST_OK;
    for (k = 0; k < n; k++) {
        if (isnan(p[k]))
            return DYN_ST_E_NAN;
        if (p[k] < 0.0 || p[k] > 1.0)
            return DYN_ST_E_DOMAIN;
    }
    if (method == DYN_ST_ADJ_BONFERRONI) {
        for (k = 0; k < n; k++)
            out[k] = fmin(1.0, p[k] * (double)n);
        return DYN_ST_OK;
    }
    idx = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!idx)
        return DYN_ST_E_OOM;
    rc = dyn_st_argsort(p, n, idx);
    if (rc != DYN_ST_OK) {
        free(idx);
        return rc;
    }
    if (method == DYN_ST_ADJ_HOLM) {
        run = 0.0;
        for (k = 0; k < n; k++) {
            double v = fmin(1.0, p[idx[k]] * (double)(n - k));
            if (v > run)
                run = v;
            out[idx[k]] = run;
        }
    } else {
        if (method == DYN_ST_ADJ_BY)
            for (k = 2; k <= n; k++)
                by += 1.0 / (double)k;
        run = 1.0;
        for (k = n; k-- > 0;) {
            double v = method == DYN_ST_ADJ_HOCHBERG ? p[idx[k]] * (double)(n - k)
                                                     : p[idx[k]] * (double)n / (double)(k + 1) * by;
            if (v < run)
                run = v;
            out[idx[k]] = run;
        }
    }
    free(idx);
    return DYN_ST_OK;
}

int dyn_st_psi_counts(const double* e, const double* a, uint32_t n, double eps, double* out)
{
    double se, sa, s = 0.0;
    uint32_t i;
    if (n == 0)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(e, n) || dyn_st_has_nan(a, n))
        return DYN_ST_E_NAN;
    for (i = 0; i < n; i++)
        if (e[i] < 0.0 || a[i] < 0.0)
            return DYN_ST_E_DOMAIN;
    se = st_sum(e, n);
    sa = st_sum(a, n);
    if (!(se > 0.0) || !(sa > 0.0))
        return DYN_ST_E_DOMAIN;
    for (i = 0; i < n; i++) {
        double pe = fmax(e[i] / se, eps), pa = fmax(a[i] / sa, eps);
        s += (pa - pe) * log(pa / pe);
    }
    *out = s;
    return DYN_ST_OK;
}

static uint32_t st_upper(const double* edges, uint32_t ne, double v)
{
    uint32_t lo = 0, hi = ne;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (edges[mid] <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int dyn_st_psi(const double* e, uint32_t ne, const double* a, uint32_t na, uint32_t bins, double eps, double* out)
{
    double *s, *edges, *ce, *ca;
    uint32_t k, i;
    int rc;
    if (ne == 0 || na == 0 || bins < 2)
        return DYN_ST_E_SIZE;
    if (dyn_st_has_nan(e, ne) || dyn_st_has_nan(a, na))
        return DYN_ST_E_NAN;
    s = (double*)malloc(((size_t)ne + 3u * bins) * sizeof(double));
    if (!s)
        return DYN_ST_E_OOM;
    edges = s + ne;
    ce = edges + bins;
    ca = ce + bins;
    rc = dyn_st_sort_copy(e, ne, s);
    if (rc != DYN_ST_OK) {
        free(s);
        return rc;
    }
    for (k = 0; k + 1 < bins; k++) {
        uint64_t at = ((uint64_t)(k + 1) * ne) / bins;
        edges[k] = s[at < ne ? at : ne - 1];
    }
    memset(ce, 0, 2u * bins * sizeof(double));
    for (i = 0; i < ne; i++)
        ce[st_upper(edges, bins - 1, e[i])] += 1.0;
    for (i = 0; i < na; i++)
        ca[st_upper(edges, bins - 1, a[i])] += 1.0;
    rc = dyn_st_psi_counts(ce, ca, bins, eps, out);
    free(s);
    return rc;
}
