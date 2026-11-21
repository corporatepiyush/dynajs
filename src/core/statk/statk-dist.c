#include "statk-int.h"

#include <math.h>

#define ST_SQRT1_2 0.70710678118654752440
#define ST_LN_SQRT_2PI 0.91893853320467274178

double dyn_st_norm_pdf(double x)
{
    return exp(-0.5 * x * x - ST_LN_SQRT_2PI);
}

double dyn_st_norm_cdf(double x)
{
    return 0.5 * erfc(-x * ST_SQRT1_2);
}

double dyn_st_norm_sf(double x)
{
    return 0.5 * erfc(x * ST_SQRT1_2);
}

static double st_ppnd_central(double q)
{
    double r = 0.180625 - q * q;
    return q
        * (((((((r * 2509.0809287301226727 + 33430.575583588128105) * r + 67265.770927008700853) * r
                  + 45921.953931549871457)
                     * r
                 + 13731.693765509461125)
                    * r
                + 1971.5909503065514427)
                   * r
               + 133.14166789178437745)
                * r
            + 3.387132872796366608)
        / (((((((r * 5226.495278852545925 + 28729.085735721942674) * r + 39307.89580009271061) * r
                  + 21213.794301586595867)
                     * r
                 + 5394.1960214247511077)
                    * r
                + 687.1870074920579083)
                   * r
               + 42.313330701600911252)
                * r
            + 1.0);
}

static double st_ppnd_tail(double r)
{
    r = sqrt(-log(r));
    if (r <= 5.0) {
        r -= 1.6;
        return (((((((r * 7.7454501427834140764e-4 + .0227238449892691845833) * r + .24178072517745061177) * r
                       + 1.27045825245236838258)
                          * r
                      + 3.64784832476320460504)
                         * r
                     + 5.7694972214606914055)
                        * r
                    + 4.6303378461565452959)
                       * r
                   + 1.42343711074968357734)
            / (((((((r * 1.05075007164441684324e-9 + 5.475938084995344946e-4) * r + .0151986665636164571966) * r
                      + .14810397642748007459)
                         * r
                     + .68976733498510000455)
                        * r
                    + 1.6763848301838038494)
                       * r
                   + 2.05319162663775882187)
                    * r
                + 1.0);
    }
    r -= 5.0;
    return (((((((r * 2.01033439929228813265e-7 + 2.71155556874348757815e-5) * r + .0012426609473880784386) * r
                   + .026532189526576123093)
                      * r
                  + .29656057182850489123)
                     * r
                 + 1.7848265399172913358)
                    * r
                + 5.4637849111641143699)
                   * r
               + 6.6579046435011037772)
        / (((((((r * 2.04426310338993978564e-15 + 1.4215117583164458887e-7) * r + 1.8463183175100546818e-5) * r
                  + 7.868691311456132591e-4)
                     * r
                 + .0148753612908506148525)
                    * r
                + .13692988092273580531)
                   * r
               + .59983220655588793769)
                * r
            + 1.0);
}

double dyn_st_norm_ppf(double p)
{
    double q;
    if (isnan(p) || p < 0.0 || p > 1.0)
        return DYN_NAN;
    if (p == 0.0)
        return -DYN_INFINITY;
    if (p == 1.0)
        return DYN_INFINITY;
    q = p - 0.5;
    if (fabs(q) <= 0.425)
        return st_ppnd_central(q);
    return q < 0.0 ? -st_ppnd_tail(p) : st_ppnd_tail(1.0 - p);
}

static int st_bad_df(double df)
{
    return isnan(df) || df <= 0.0;
}

double dyn_st_t_pdf(double x, double df)
{
    if (isnan(x) || st_bad_df(df))
        return DYN_NAN;
    if (isinf(df))
        return dyn_st_norm_pdf(x);
    return exp(dyn_st_lgamma(0.5 * (df + 1.0)) - dyn_st_lgamma(0.5 * df) - 0.5 * log(df * M_PI)
        - 0.5 * (df + 1.0) * log1p(x * x / df));
}

double dyn_st_t_cdf(double x, double df)
{
    double x2, tail;
    if (isnan(x) || st_bad_df(df))
        return DYN_NAN;
    if (isinf(df))
        return dyn_st_norm_cdf(x);
    if (isinf(x))
        return x > 0 ? 1.0 : 0.0;
    x2 = x * x;
    if (x2 < 1.0) {
        double c = 0.5 * dyn_betainc(x2 / (df + x2), 0.5, 0.5 * df);
        return x < 0 ? 0.5 - c : 0.5 + c;
    }
    tail = 0.5 * dyn_betainc(df / (df + x2), 0.5 * df, 0.5);
    return x > 0 ? 1.0 - tail : tail;
}

double dyn_st_t_sf(double x, double df)
{
    return dyn_st_t_cdf(-x, df);
}

double dyn_st_t_ppf(double p, double df)
{
    double q, t;
    int i;
    if (isnan(p) || p < 0.0 || p > 1.0 || st_bad_df(df))
        return DYN_NAN;
    if (isinf(df))
        return dyn_st_norm_ppf(p);
    if (p == 0.0)
        return -DYN_INFINITY;
    if (p == 1.0)
        return DYN_INFINITY;
    if (p == 0.5)
        return 0.0;
    q = p < 0.5 ? p : 1.0 - p;
    if (q < 0.25) {
        double z = dyn_betaincinv(2.0 * q, 0.5 * df, 0.5);
        t = sqrt(df * ((1.0 - z) / z));
    } else {
        double w = dyn_betaincinv(1.0 - 2.0 * q, 0.5, 0.5 * df);
        t = sqrt(df * (w / (1.0 - w)));
    }
    if (p < 0.5)
        t = -t;
    for (i = 0; i < 3 && isfinite(t); i++) {
        double d = dyn_st_t_pdf(t, df), step;
        if (!(d > 0.0))
            break;
        step = p < 0.5 ? (dyn_st_t_cdf(t, df) - p) / d : ((1.0 - p) - dyn_st_t_sf(t, df)) / d;
        t -= step;
        if (fabs(step) <= 1e-15 * fabs(t))
            break;
    }
    return t;
}

double dyn_st_chi2_pdf(double x, double k)
{
    double h;
    if (isnan(x) || st_bad_df(k))
        return DYN_NAN;
    if (x < 0.0)
        return 0.0;
    h = 0.5 * k;
    if (x == 0.0)
        return h < 1.0 ? DYN_INFINITY : (h == 1.0 ? 0.5 : 0.0);
    return exp((h - 1.0) * log(x) - 0.5 * x - h * M_LN2 - dyn_st_lgamma(h));
}

double dyn_st_chi2_cdf(double x, double k)
{
    if (isnan(x) || st_bad_df(k))
        return DYN_NAN;
    if (x <= 0.0)
        return 0.0;
    return dyn_gammainc_p(0.5 * k, 0.5 * x);
}

double dyn_st_chi2_sf(double x, double k)
{
    if (isnan(x) || st_bad_df(k))
        return DYN_NAN;
    if (x <= 0.0)
        return 1.0;
    return dyn_gammainc_q(0.5 * k, 0.5 * x);
}

double dyn_st_chi2_ppf(double p, double k)
{
    if (isnan(p) || p < 0.0 || p > 1.0 || st_bad_df(k))
        return DYN_NAN;
    if (p == 0.0)
        return 0.0;
    if (p == 1.0)
        return DYN_INFINITY;
    return 2.0 * dyn_gammaincinv(0.5 * k, p);
}

double dyn_st_f_cdf(double x, double d1, double d2)
{
    double y;
    if (isnan(x) || st_bad_df(d1) || st_bad_df(d2))
        return DYN_NAN;
    if (x <= 0.0)
        return 0.0;
    if (isinf(x))
        return 1.0;
    y = d1 * x;
    return dyn_betainc(y / (y + d2), 0.5 * d1, 0.5 * d2);
}

double dyn_st_f_sf(double x, double d1, double d2)
{
    double y;
    if (isnan(x) || st_bad_df(d1) || st_bad_df(d2))
        return DYN_NAN;
    if (x <= 0.0)
        return 1.0;
    if (isinf(x))
        return 0.0;
    y = d1 * x;
    return dyn_betainc(d2 / (d2 + y), 0.5 * d2, 0.5 * d1);
}

double dyn_st_f_ppf(double p, double d1, double d2)
{
    double z;
    if (isnan(p) || p < 0.0 || p > 1.0 || st_bad_df(d1) || st_bad_df(d2))
        return DYN_NAN;
    if (p == 0.0)
        return 0.0;
    if (p == 1.0)
        return DYN_INFINITY;
    z = dyn_betaincinv(p, 0.5 * d1, 0.5 * d2);
    return d2 * z / (d1 * (1.0 - z));
}

static int st_bad_binom(double n, double p)
{
    return isnan(n) || isnan(p) || n < 0.0 || n != floor(n) || p < 0.0 || p > 1.0;
}

double dyn_st_binom_pmf(double k, double n, double p)
{
    if (isnan(k) || st_bad_binom(n, p))
        return DYN_NAN;
    if (k != floor(k) || k < 0.0 || k > n)
        return 0.0;
    if (p == 0.0)
        return k == 0.0 ? 1.0 : 0.0;
    if (p == 1.0)
        return k == n ? 1.0 : 0.0;
    return exp(dyn_st_lgamma(n + 1.0) - dyn_st_lgamma(k + 1.0) - dyn_st_lgamma(n - k + 1.0) + k * log(p) + (n - k) * log1p(-p));
}

double dyn_st_binom_cdf(double k, double n, double p)
{
    if (isnan(k) || st_bad_binom(n, p))
        return DYN_NAN;
    k = floor(k);
    if (k < 0.0)
        return 0.0;
    if (k >= n)
        return 1.0;
    if (p == 0.0)
        return 1.0;
    if (p == 1.0)
        return 0.0;
    return dyn_betainc(1.0 - p, n - k, k + 1.0);
}

double dyn_st_binom_sf(double k, double n, double p)
{
    if (isnan(k) || st_bad_binom(n, p))
        return DYN_NAN;
    k = floor(k);
    if (k < 0.0)
        return 1.0;
    if (k >= n)
        return 0.0;
    if (p == 0.0)
        return 0.0;
    if (p == 1.0)
        return 1.0;
    return dyn_betainc(p, k + 1.0, n - k);
}

double dyn_st_poisson_pmf(double k, double mu)
{
    if (isnan(k) || isnan(mu) || mu < 0.0)
        return DYN_NAN;
    if (k != floor(k) || k < 0.0)
        return 0.0;
    if (mu == 0.0)
        return k == 0.0 ? 1.0 : 0.0;
    return exp(k * log(mu) - mu - dyn_st_lgamma(k + 1.0));
}

double dyn_st_poisson_cdf(double k, double mu)
{
    if (isnan(k) || isnan(mu) || mu < 0.0)
        return DYN_NAN;
    k = floor(k);
    if (k < 0.0)
        return 0.0;
    if (mu == 0.0)
        return 1.0;
    return dyn_gammainc_q(k + 1.0, mu);
}

double dyn_st_poisson_sf(double k, double mu)
{
    if (isnan(k) || isnan(mu) || mu < 0.0)
        return DYN_NAN;
    k = floor(k);
    if (k < 0.0)
        return 1.0;
    if (mu == 0.0)
        return 0.0;
    return dyn_gammainc_p(k + 1.0, mu);
}

double dyn_st_kolmogorov_sf(double x)
{
    double s = 0.0;
    int k;
    if (isnan(x))
        return DYN_NAN;
    if (x <= 0.0)
        return 1.0;
    if (x < 1.0) {
        double w = M_PI * M_PI / (8.0 * x * x), c = 0.0;
        for (k = 1; k < 64; k++) {
            double m = (double)(2 * k - 1), t = exp(-m * m * w);
            c += t;
            if (t < 1e-17 * c)
                break;
        }
        return 1.0 - sqrt(2.0 * M_PI) / x * c;
    }
    for (k = 1; k < 101; k++) {
        double t = exp(-2.0 * (double)k * (double)k * x * x);
        s += (k & 1) ? t : -t;
        if (t < 1e-17)
            break;
    }
    s *= 2.0;
    return s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
}
