#include "dyn-mathx.h"
#include "dyn-cdefs.h"

#include <float.h>
#include <limits.h>
#define _REENTRANT
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MX_EPS 3.0e-16
#define MX_FPMIN 1.0e-300
#define MX_ITMAX 400
#define MX_ITMAX_CEIL 5000000.0

static int mx_iter_budget(double shape)
{
    double want = (double)MX_ITMAX + 12.0 * sqrt(shape > 0.0 ? shape : 0.0);
    return want > MX_ITMAX_CEIL ? (int)MX_ITMAX_CEIL : (int)want;
}

static double mx_lgamma_raw(double x)
{
    int sign;
#if defined(_WIN32)
    sign = 0;
    (void)sign;
    return lgamma(x);
#else
    return lgamma_r(x, &sign);
#endif
}

#define MX_LG_SLOTS 8

typedef struct {
    uint64_t key;
    double val;
} mx_lg_slot_t;

static _Thread_local mx_lg_slot_t mx_lg_cache[MX_LG_SLOTS];

double dyn_lgamma_cached(double x)
{
    uint64_t u;
    mx_lg_slot_t* s;
    if (x != x)
        return mx_lgamma_raw(x);
    memcpy(&u, &x, sizeof u);
    s = &mx_lg_cache[(u * 0x9e3779b97f4a7c15ull) >> 61];
    if (s->key != ~u) {
        s->val = mx_lgamma_raw(x);
        s->key = ~u;
    }
    return s->val;
}

static double mx_lgamma(double x)
{
    return dyn_lgamma_cached(x);
}

double dyn_stirling_tail(double z)
{
    static const double c[] = { 1.0 / 12.0, -1.0 / 360.0, 1.0 / 1260.0,
        -1.0 / 1680.0, 1.0 / 1188.0, -691.0 / 360360.0, 1.0 / 156.0,
        -3617.0 / 122400.0 };
    double r = 1.0 / z, r2 = r * r, acc = 0.0;
    int k = (int)(sizeof(c) / sizeof(c[0]));
    while (k-- > 0)
        acc = acc * r2 + c[k];
    return acc * r;
}

static double mx_log1pmx(double t)
{
    double term, sum = 0.0, pw;
    int k;
    if (fabs(t) >= 0.5)
        return log1p(t) - t;
    pw = t * t;
    for (k = 2; k < 80; k++) {
        term = pw / (double)k;
        sum += (k & 1) ? term : -term;
        if (fabs(term) < fabs(sum) * 1.0e-17)
            break;
        pw *= t;
    }
    return sum;
}

static double mx_scaled_log1pmx(double s, double num, double den)
{
    double u = (num - den) / den;
    if (fabs(u) < 0.5)
        return s * mx_log1pmx(u);
    return s * log(num / den) - s * u;
}

static const double MX_ZETA_MINUS_ONE_FROM_2[] = {
    0.6449340668482264, 0.2020569031595943, 0.08232323371113819,
    0.03692775514336993, 0.01734306198444914, 0.008349277381922827,
    0.00407735619794434, 0.0020083928260822143, 0.0009945751278180853,
    0.0004941886041194645, 0.0002460865533080483, 0.00012271334757848915,
    6.124813505870483e-05, 3.058823630702049e-05, 1.528225940865187e-05,
    7.637197637899763e-06, 3.81729326499984e-06, 1.908212716553939e-06,
    9.539620338727962e-07, 4.769329867878064e-07, 2.38450502727733e-07,
    1.1921992596531106e-07, 5.960818905125948e-08, 2.980350351465228e-08,
    1.4901554828365043e-08, 7.45071178983543e-09, 3.725334024788457e-09,
    1.862659723513049e-09, 9.313274324196682e-10, 4.656629065033784e-10,
    2.3283118336765053e-10, 1.164155017270052e-10, 5.820772087902701e-11
};

static double mx_lgamma1p(double a)
{
    double acc = 0.0;
    int k;
    if (fabs(a) >= 0.5)
        return mx_lgamma(1.0 + a);
    k = (int)(sizeof(MX_ZETA_MINUS_ONE_FROM_2) / sizeof(MX_ZETA_MINUS_ONE_FROM_2[0]));
    while (k-- > 0)
        acc = acc * -a + MX_ZETA_MINUS_ONE_FROM_2[k] / (double)(k + 2);
    return -0.5772156649015329 * a - mx_log1pmx(a) + acc * a * a;
}

static double mx_gammainc_q_small(double a, double x)
{
    double l = a * log(x) - mx_lgamma1p(a), pa = exp(l), s = 0.0, term = 1.0;
    int n;
    for (n = 1; n < 200; n++) {
        double add;
        term *= -x / (double)n;
        add = term / (a + (double)n);
        s += add;
        if (fabs(add) < fabs(s) * 1.0e-17)
            break;
    }
    return -expm1(l) - pa * a * s;
}

double dyn_lgamma_rise(double a, double b)
{
    return (a + b - 0.5) * log1p(b / a) + b * (log(a) - 1.0)
        + (dyn_stirling_tail(a + b) - dyn_stirling_tail(a));
}

double dyn_gamma(double x)
{
    double r, s, g;
    if (!(x < 0.5) || x == 0.0)
        return tgamma(x);
    if (x == floor(x))
        return DYN_NAN;
    r = x - nearbyint(x);
    s = sin(M_PI * r);
    if (fmod(nearbyint(x), 2.0) != 0.0)
        s = -s;
    g = tgamma(1.0 - x);
    if (isfinite(g))
        return M_PI / (s * g);
    g = exp(log(M_PI / fabs(s)) - mx_lgamma(1.0 - x));
    return s < 0.0 ? -g : g;
}

static double mx_gamma_prefix(double a, double x)
{
    if (a < 10.0)
        return exp(-x + a * log(x) - mx_lgamma(a));
    return exp(mx_scaled_log1pmx(a, x, a) + 0.5 * log(a / (2.0 * M_PI))
        - dyn_stirling_tail(a));
}

static double mx_beta_prefix(double x, double a, double b)
{
    double c, u, v;
    if (a < 10.0 && b < 10.0)
        return exp(mx_lgamma(a + b) - mx_lgamma(a) - mx_lgamma(b) + a * log(x) + b * log1p(-x));
    if (a < 10.0)
        return exp(dyn_lgamma_rise(b, a) - mx_lgamma(a) + a * log(x) + b * log1p(-x));
    if (b < 10.0)
        return exp(dyn_lgamma_rise(a, b) - mx_lgamma(b) + a * log(x) + b * log1p(-x));
    c = a + b;
    u = (x * c - a) / a;
    v = ((1.0 - x) * c - b) / b;
    return exp((fabs(u) < 0.5 ? a * mx_log1pmx(u) : a * log(x * c / a) - a * u)
        + (fabs(v) < 0.5 ? b * mx_log1pmx(v) : b * log1p(-x) + b * log(c / b) - b * v)
        + 0.5 * log(a / (2.0 * M_PI) * (b / c))
        + dyn_stirling_tail(c) - dyn_stirling_tail(a) - dyn_stirling_tail(b));
}

static double mx_gser(double a, double x)
{
    double sum, del, ap;
    int n, itmax;

    if (x <= 0.0)
        return 0.0;

    ap = a;
    del = sum = 1.0 / a;
    itmax = mx_iter_budget(a > x ? a : x);
    for (n = 0; n < itmax; n++) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (fabs(del) < fabs(sum) * MX_EPS)
            break;
    }
    if (n == itmax)
        return DYN_NAN;
    return sum * mx_gamma_prefix(a, x);
}

static double mx_gcf(double a, double x)
{
    double b, c, d, h, an, del;
    int i, itmax = mx_iter_budget(a > x ? a : x);

    b = x + 1.0 - a;
    c = 1.0 / MX_FPMIN;
    d = 1.0 / b;
    h = d;
    for (i = 1; i <= itmax; i++) {
        an = -(double)i * ((double)i - a);
        b += 2.0;
        d = an * d + b;
        if (fabs(d) < MX_FPMIN)
            d = MX_FPMIN;
        c = b + an / c;
        if (fabs(c) < MX_FPMIN)
            c = MX_FPMIN;
        d = 1.0 / d;
        del = d * c;
        h *= del;
        if (fabs(del - 1.0) < MX_EPS)
            break;
    }
    if (i > itmax)
        return DYN_NAN;
    return mx_gamma_prefix(a, x) * h;
}

double dyn_gammainc_p(double a, double x)
{
    if (!(x >= 0.0) || !(a > 0.0))
        return DYN_NAN;
    if (x == 0.0)
        return 0.0;
    if (x < a + 1.0)
        return mx_gser(a, x);
    return 1.0 - mx_gcf(a, x);
}

double dyn_gammainc_q(double a, double x)
{
    if (!(x >= 0.0) || !(a > 0.0))
        return DYN_NAN;
    if (x == 0.0)
        return 1.0;
    if (a < 1.0 && x < 1.1)
        return mx_gammainc_q_small(a, x);
    if (x < a + 1.0)
        return 1.0 - mx_gser(a, x);
    return mx_gcf(a, x);
}

double dyn_gammaincinv(double a, double p)
{
    double x, err, t, u, a1, lna1, afac, pp, lga;
    int j;

    if (!(a > 0.0) || !(p >= 0.0) || !(p <= 1.0))
        return DYN_NAN;
    if (p == 0.0)
        return 0.0;
    if (p == 1.0)
        return DYN_INFINITY;

    a1 = a - 1.0;
    lna1 = 0.0;
    afac = 0.0;
    if (a > 1.0) {
        lna1 = log(a1);
        afac = exp(a1 * (lna1 - 1.0) - mx_lgamma(a));
        pp = (p < 0.5) ? p : 1.0 - p;
        t = sqrt(-2.0 * log(pp));
        x = (2.30753 + t * 0.27061) / (1.0 + t * (0.99229 + t * 0.04481)) - t;
        if (p < 0.5)
            x = -x;
        x = a * pow(1.0 - 1.0 / (9.0 * a) - x / (3.0 * sqrt(a)), 3.0);
        if (x < 1.0e-3)
            x = 1.0e-3;
    } else {
        lga = mx_lgamma(a);
        t = 1.0 - a * (0.253 + a * 0.12);
        if (p < t)
            x = pow(p / t, 1.0 / a);
        else
            x = 1.0 - log(1.0 - (p - t) / (1.0 - t));
    }

    for (j = 0; j < 24; j++) {
        if (x <= 0.0)
            return 0.0;
        err = (p <= 0.5) ? dyn_gammainc_p(a, x) - p : (1.0 - p) - dyn_gammainc_q(a, x);
        if (a > 1.0)
            t = afac * exp(-(x - a1) + a1 * (log(x) - lna1));
        else
            t = exp(-x + a1 * log(x) - lga);
        if (t == 0.0)
            break;
        u = err / t;
        {
            double corr = u * ((a - 1.0) / x - 1.0);
            if (corr > 1.0)
                corr = 1.0;
            t = u / (1.0 - 0.5 * corr);
        }
        x -= t;
        if (x <= 0.0)
            x = 0.5 * (x + t);
        if (fabs(t) < MX_EPS * x)
            break;
    }
    return x;
}

static double mx_betacf(double a, double b, double x)
{
    double qab, qap, qam, c, d, h, aa, del, m2;
    int m, itmax = mx_iter_budget(a > b ? a : b);

    qab = a + b;
    qap = a + 1.0;
    qam = a - 1.0;
    c = 1.0;
    d = 1.0 - qab * x / qap;
    if (fabs(d) < MX_FPMIN)
        d = MX_FPMIN;
    d = 1.0 / d;
    h = d;
    for (m = 1; m <= itmax; m++) {
        m2 = 2.0 * (double)m;
        aa = (double)m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < MX_FPMIN)
            d = MX_FPMIN;
        c = 1.0 + aa / c;
        if (fabs(c) < MX_FPMIN)
            c = MX_FPMIN;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < MX_FPMIN)
            d = MX_FPMIN;
        c = 1.0 + aa / c;
        if (fabs(c) < MX_FPMIN)
            c = MX_FPMIN;
        d = 1.0 / d;
        del = d * c;
        h *= del;
        if (fabs(del - 1.0) < MX_EPS)
            break;
    }
    if (m > itmax)
        return DYN_NAN;
    return h;
}

double dyn_betainc(double x, double a, double b)
{
    double bt;

    if (!(a > 0.0) || !(b > 0.0) || !(x >= 0.0) || !(x <= 1.0))
        return DYN_NAN;
    if (x == 0.0)
        return 0.0;
    if (x == 1.0)
        return 1.0;

    bt = mx_beta_prefix(x, a, b);
    if (x < (a + 1.0) / (a + b + 2.0))
        return bt * mx_betacf(a, b, x) / a;
    return 1.0 - bt * mx_betacf(b, a, 1.0 - x) / b;
}

double dyn_betaincinv(double p, double a, double b)
{
    double x, t, u, w, err, a1, b1, afac, pp, al, h;
    int j;

    if (!(a > 0.0) || !(b > 0.0) || !(p >= 0.0) || !(p <= 1.0))
        return DYN_NAN;
    if (p <= 0.0)
        return 0.0;
    if (p >= 1.0)
        return 1.0;

    a1 = a - 1.0;
    b1 = b - 1.0;

    if (a >= 1.0 && b >= 1.0) {
        pp = (p < 0.5) ? p : 1.0 - p;
        t = sqrt(-2.0 * log(pp));
        x = (2.30753 + t * 0.27061) / (1.0 + t * (0.99229 + t * 0.04481)) - t;
        if (p < 0.5)
            x = -x;
        al = (x * x - 3.0) / 6.0;
        h = 2.0 / (1.0 / (2.0 * a - 1.0) + 1.0 / (2.0 * b - 1.0));
        w = (x * sqrt(al + h) / h) - (1.0 / (2.0 * b - 1.0) - 1.0 / (2.0 * a - 1.0)) * (al + 5.0 / 6.0 - 2.0 / (3.0 * h));
        x = a / (a + b * exp(2.0 * w));
    } else {
        double lna = log(a / (a + b)), lnb = log(b / (a + b));
        t = exp(a * lna) / a;
        u = exp(b * lnb) / b;
        w = t + u;
        if (p < t / w)
            x = pow(a * w * p, 1.0 / a);
        else
            x = 1.0 - pow(b * w * (1.0 - p), 1.0 / b);
    }

    afac = -mx_lgamma(a) - mx_lgamma(b) + mx_lgamma(a + b);
    for (j = 0; j < 20; j++) {
        if (x == 0.0 || x == 1.0)
            return x;
        err = dyn_betainc(x, a, b) - p;
        t = exp(a1 * log(x) + b1 * log1p(-x) + afac);
        if (t == 0.0)
            break;
        u = err / t;
        {
            double corr = u * (a1 / x - b1 / (1.0 - x));
            if (corr > 1.0)
                corr = 1.0;
            t = u / (1.0 - 0.5 * corr);
        }
        x -= t;
        if (x <= 0.0)
            x = 0.5 * (x + t);
        if (x >= 1.0)
            x = 0.5 * (x + t + 1.0);
        if (fabs(t) < MX_EPS * x && j > 0)
            break;
    }
    return x;
}

#define MX_I_SERIES_MAX 20.0
#define MX_I_SERIES_X_CEIL 1.0e6
#define MX_I_ASYM_FIRST_TERM_MAX 0.5

typedef struct {
    double hi, lo;
} mx_dd;

static mx_dd mx_dd_fast(double a, double b)
{
    double s = a + b;
    mx_dd r;
    r.hi = s;
    r.lo = b - (s - a);
    return r;
}

static mx_dd mx_dd_sum(double a, double b)
{
    double s = a + b, bb = s - a;
    mx_dd r;
    r.hi = s;
    r.lo = (a - (s - bb)) + (b - bb);
    return r;
}

static mx_dd mx_dd_prod(double a, double b)
{
    mx_dd r;
    r.hi = a * b;
    r.lo = fma(a, b, -r.hi);
    return r;
}

static mx_dd mx_dd_add(mx_dd a, mx_dd b)
{
    mx_dd s = mx_dd_sum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return mx_dd_fast(s.hi, s.lo);
}

static mx_dd mx_dd_neg(mx_dd a)
{
    a.hi = -a.hi;
    a.lo = -a.lo;
    return a;
}

static mx_dd mx_dd_mul(mx_dd a, mx_dd b)
{
    mx_dd p = mx_dd_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return mx_dd_fast(p.hi, p.lo);
}

static mx_dd mx_dd_muld(mx_dd a, double b)
{
    mx_dd p = mx_dd_prod(a.hi, b);
    p.lo += a.lo * b;
    return mx_dd_fast(p.hi, p.lo);
}

static mx_dd mx_dd_div(mx_dd a, mx_dd b)
{
    double q1 = a.hi / b.hi, r;
    mx_dd p = mx_dd_prod(q1, b.hi);
    r = (((a.hi - p.hi) - p.lo) + a.lo - q1 * b.lo) / b.hi;
    return mx_dd_fast(q1, r);
}

static mx_dd mx_dd_divd(mx_dd a, double b)
{
    double q1 = a.hi / b, r;
    mx_dd p = mx_dd_prod(q1, b);
    r = (((a.hi - p.hi) - p.lo) + a.lo) / b;
    return mx_dd_fast(q1, r);
}

static mx_dd mx_dd_c(double hi, double lo)
{
    mx_dd r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

static mx_dd mx_dd_norm(mx_dd a, int64_t* e)
{
    int k;
    if (a.hi == 0.0 || !isfinite(a.hi))
        return a;
    (void)frexp(a.hi, &k);
    *e += k;
    a.hi = ldexp(a.hi, -k);
    a.lo = ldexp(a.lo, -k);
    return a;
}

static double mx_dd_ldexp(mx_dd a, int64_t e)
{
    double v = a.hi + a.lo;
    if (e > 4096)
        e = 4096;
    if (e < -4096)
        e = -4096;
    if (e > 1000 || e < -1000) {
        v = ldexp(v, (int)(e / 2));
        return ldexp(v, (int)(e - e / 2));
    }
    return ldexp(v, (int)e);
}

static mx_dd mx_dd_sqrt(mx_dd a)
{
    double r = sqrt(a.hi);
    return mx_dd_fast(r, (fma(-r, r, a.hi) + a.lo) / (2.0 * r));
}

#define MX_I_DD_NU_MAX 2000.0

static int mx_besseli_pref_dd(double nu, double h, double x, int scaled, mx_dd* m, int64_t* e)
{
    mx_dd r = mx_dd_c(1.0, 0.0), g;
    int64_t er = 0, eg = 0;
    double f;
    uint32_t n, k;
    int eh;
    if (!(nu > -1.0) || nu > MX_I_DD_NU_MAX)
        return -1;
    if (nu < 0.0) {
        r = mx_dd_c(pow(h, nu), 0.0);
        g = mx_dd_c(tgamma(nu + 1.0), 0.0);
        if (!isfinite(r.hi) || r.hi == 0.0)
            return -1;
    } else {
        n = (uint32_t)floor(nu);
        f = nu - (double)n;
        {
            int64_t eb = 0;
            mx_dd base = mx_dd_c(frexp(h, &eh), 0.0);
            r = mx_dd_c(1.0, 0.0);
            er = 0;
            for (k = n; k; k >>= 1) {
                if (k & 1) {
                    r = mx_dd_mul(r, base);
                    er += eb;
                    r = mx_dd_norm(r, &er);
                }
                base = mx_dd_mul(base, base);
                eb *= 2;
                base = mx_dd_norm(base, &eb);
            }
            er += (int64_t)eh * (int64_t)n;
        }
        if (f > 0.0)
            r = mx_dd_norm(mx_dd_muld(r, pow(h, f)), &er);
        {
            double gh[4] = { f > 0.0 ? tgamma(1.0 + f) : 1.0, 1.0, 1.0, 1.0 }, gl[4] = { 0.0, 0.0, 0.0, 0.0 };
            double h0 = gh[0], h1 = 1.0, h2 = 1.0, h3 = 1.0, l0 = 0.0, l1 = 0.0, l2 = 0.0, l3 = 0.0;
            int64_t ge[4] = { 0, 0, 0, 0 };
            int j;
            k = 1;
            while (k <= n) {
                uint32_t lim = k + 124 < n ? k + 124 : n;
                for (; k + 3 <= lim; k += 4) {
                    double a0 = f + (double)k, a1 = f + (double)(k + 1), a2 = f + (double)(k + 2), a3 = f + (double)(k + 3);
                    double b0 = f - (a0 - (double)k), b1 = f - (a1 - (double)(k + 1)), b2 = f - (a2 - (double)(k + 2)), b3 = f - (a3 - (double)(k + 3));
                    double n0 = h0 * a0, n1 = h1 * a1, n2 = h2 * a2, n3 = h3 * a3;
                    l0 = fma(h0, a0, -n0) + l0 * a0 + h0 * b0;
                    l1 = fma(h1, a1, -n1) + l1 * a1 + h1 * b1;
                    l2 = fma(h2, a2, -n2) + l2 * a2 + h2 * b2;
                    l3 = fma(h3, a3, -n3) + l3 * a3 + h3 * b3;
                    h0 = n0;
                    h1 = n1;
                    h2 = n2;
                    h3 = n3;
                }
                for (; k <= lim; k++) {
                    double a0 = f + (double)k, b0 = f - (a0 - (double)k), n0 = h0 * a0;
                    l0 = fma(h0, a0, -n0) + l0 * a0 + h0 * b0;
                    h0 = n0;
                }
                gh[0] = h0;
                gh[1] = h1;
                gh[2] = h2;
                gh[3] = h3;
                gl[0] = l0;
                gl[1] = l1;
                gl[2] = l2;
                gl[3] = l3;
                for (j = 0; j < 4; j++) {
                    int sh;
                    (void)frexp(gh[j], &sh);
                    gh[j] = ldexp(gh[j], -sh);
                    gl[j] = ldexp(gl[j], -sh);
                    ge[j] += sh;
                }
                h0 = gh[0];
                h1 = gh[1];
                h2 = gh[2];
                h3 = gh[3];
                l0 = gl[0];
                l1 = gl[1];
                l2 = gl[2];
                l3 = gl[3];
            }
            {
                mx_dd p0 = mx_dd_mul(mx_dd_fast(gh[0], gl[0]), mx_dd_fast(gh[1], gl[1]));
                mx_dd p1 = mx_dd_mul(mx_dd_fast(gh[2], gl[2]), mx_dd_fast(gh[3], gl[3]));
                int64_t e0 = ge[0] + ge[1], e1 = ge[2] + ge[3];
                p0 = mx_dd_norm(p0, &e0);
                p1 = mx_dd_norm(p1, &e1);
                g = mx_dd_mul(p0, p1);
                eg += e0 + e1;
            }
        }
    }
    g = mx_dd_norm(g, &eg);
    r = mx_dd_norm(r, &er);
    *m = mx_dd_div(r, g);
    *e = er - eg;
    if (scaled) {
        double kk = floor(x / 0.6931471805599453), rr;
        rr = fma(-kk, 0.6931471805599453, x);
        rr = fma(-kk, 2.3190468138462996e-17, rr);
        *m = mx_dd_muld(*m, exp(-rr));
        *e -= (int64_t)kk;
    }
    *m = mx_dd_norm(*m, e);
    return 0;
}

static double mx_besseli_series_dd(double nu, double x, int scaled)
{
    double h = 0.5 * x, q = h * h, ql = fma(h, h, -q), iq = 1.0 / q;
    double tt = 1.0, tc = 0.0, s = 1.0, se_ = 0.0, sc = 0.0;
    mx_dd sum, pm;
    int64_t se = 0, pe = 0;
    int i, itmax;
    if (mx_besseli_pref_dd(nu, h, x, scaled, &pm, &pe) < 0)
        return DYN_NAN;
    itmax = 300 + (int)(2.0 * x);
    if (q > 0x1p-1000) {
        for (i = 1; i < itmax; i++) {
            double k = (double)i, sk = nu + k, skl, d, dl, r, dr, tn, ns, bb;
            skl = (nu - (sk - (sk - nu))) + (k - (sk - nu));
            d = k * sk;
            dl = fma(k, sk, -d) + k * skl;
            r = q / d;
            dr = (fma(-r, d, q) + ql - r * dl) * iq;
            tn = tt * r;
            tc = fma(tt, r, -tn) + tc * r + tn * dr;
            tt = tn;
            ns = s + tt;
            bb = ns - s;
            se_ += (s - (ns - bb)) + (tt - bb);
            s = ns;
            sc += tc;
            if (tt < s * 1.0e-20)
                break;
            if (s > 0x1p800) {
                s = ldexp(s, -800);
                se_ = ldexp(se_, -800);
                sc = ldexp(sc, -800);
                tt = ldexp(tt, -800);
                tc = ldexp(tc, -800);
                se += 800;
            }
        }
        if (i == itmax)
            return DYN_NAN;
    }
    sum = mx_dd_fast(s, se_ + sc);
    sum = mx_dd_norm(sum, &se);
    return mx_dd_ldexp(mx_dd_mul(sum, pm), se + pe);
}

static double mx_besseli_series_log(double nu, double x, int scaled)
{
    double half = 0.5 * x, term = 1.0, sum = 1.0, k, logscale;
    int i, itmax;

    if (x > MX_I_SERIES_X_CEIL)
        return DYN_NAN;
    itmax = 300 + (int)(2.0 * x);
    logscale = nu * log(half) - mx_lgamma(nu + 1.0);
    for (i = 1; i < itmax; i++) {
        k = (double)i;
        term *= (half * half) / (k * (nu + k));
        sum += term;
        if (term < sum * 1.0e-18)
            break;
        if (sum > 1.0e250) {
            sum *= 1.0e-250;
            term *= 1.0e-250;
            logscale += 250.0 * M_LN10;
        }
    }
    if (i == itmax)
        return DYN_NAN;
    return sum * exp(scaled ? logscale - x : logscale);
}

static double mx_besseli_series(double nu, double x, int scaled)
{
    double v = mx_besseli_series_dd(nu, x, scaled);
    return isnan(v) ? mx_besseli_series_log(nu, x, scaled) : v;
}

static double mx_besseli_asym(double nu, double x, int scaled)
{
    double mu = 4.0 * nu * nu, sum = 1.0, term = 1.0, prev = HUGE_VAL;
    int k;

    for (k = 1; k < 40; k++) {
        double kk = (double)k;
        double f = (mu - (2.0 * kk - 1.0) * (2.0 * kk - 1.0)) / (kk * 8.0 * x);
        term *= -f;
        if (fabs(term) > prev)
            break;
        prev = fabs(term);
        sum += term;
        if (fabs(term) < fabs(sum) * 1.0e-18)
            break;
    }
    {
        double pref = 1.0 / sqrt(2.0 * M_PI * x);
        if (scaled)
            return pref * sum;
        if (x > 700.0) {
            double eh = exp(0.5 * x);
            return eh * (pref * sum) * eh;
        }
        return exp(x) * pref * sum;
    }
}

#define MX_DEBYE_NU 40.0
#define MX_DEBYE_NU_MAX 1.0e6
#define MX_DEBYE_I_SERIES_NU 150.0
#define MX_DEBYE_TERMS 10

static const double MX_DEBYE_U[65] = {
    0.125,
    -0.20833333333333334,
    0.0703125,
    -0.4010416666666667,
    0.3342013888888889,
    0.0732421875,
    -0.8912109375,
    1.8464626736111112,
    -1.0258125964506173,
    0.112152099609375,
    -2.3640869140625,
    8.78912353515625,
    -11.207002616222994,
    4.669584423426247,
    0.22710800170898438,
    -7.368794359479632,
    42.53499874538846,
    -91.81824154324002,
    84.63621767460073,
    -28.212072558200244,
    0.5725014209747314,
    -26.491430486951554,
    218.1905117442116,
    -699.5796273761325,
    1059.9904525279999,
    -765.2524681411817,
    212.57013003921713,
    1.7277275025844574,
    -108.09091978839466,
    1200.9029132163525,
    -5305.646978613403,
    11655.393336864534,
    -13586.550006434138,
    8061.722181737309,
    -1919.457662318407,
    6.074042001273483,
    -493.915304773088,
    7109.514302489364,
    -41192.65496889755,
    122200.46498301746,
    -203400.17728041555,
    192547.00123253153,
    -96980.59838863752,
    20204.29133096615,
    24.380529699556064,
    -2499.8304818112097,
    45218.76898136273,
    -331645.1724845636,
    1268365.2733216248,
    -2813563.226586534,
    3763271.297656404,
    -2998015.9185381066,
    1311763.6146629772,
    -242919.18790055133,
    110.01714026924674,
    -13886.08975371704,
    308186.4046126624,
    -2785618.1280864547,
    13288767.166421818,
    -37567176.66076335,
    66344512.27472903,
    -74105148.21153265,
    50952602.49266464,
    -19706819.118432228,
    3284469.853072038,
};

static const double MX_LOG_ODD[15] = {
    1.0,
    0.3333333333333333,
    0.2,
    0.14285714285714285,
    0.1111111111111111,
    0.09090909090909091,
    0.07692307692307693,
    0.06666666666666667,
    0.058823529411764705,
    0.05263157894736842,
    0.047619047619047616,
    0.043478260869565216,
    0.04,
    0.037037037037037035,
    0.034482758620689655,
};

static const mx_dd MX_LOG_ODD_DD[6] = {
    { .hi = 1.0, .lo = 0.0 },
    { .hi = 0x1.5555555555555p-2, .lo = 0x1.5555555555555p-56 },
    { .hi = 0x1.999999999999ap-3, .lo = -0x1.999999999999ap-57 },
    { .hi = 0x1.2492492492492p-3, .lo = 0x1.2492492492492p-57 },
    { .hi = 0x1.c71c71c71c71cp-4, .lo = 0x1.c71c71c71c71cp-58 },
    { .hi = 0x1.745d1745d1746p-4, .lo = -0x1.745d1745d1746p-59 },
};

static const mx_dd MX_LN2_DD = { .hi = 0.6931471805599453, .lo = 2.3190468138462996e-17 };
static const mx_dd MX_TWO_PI_DD = { .hi = 6.283185307179586, .lo = 2.4492935982947064e-16 };
static const mx_dd MX_HALF_PI_DD = { .hi = 1.5707963267948966, .lo = 6.123233995736766e-17 };

static mx_dd mx_dd_log(mx_dd a, int64_t e2)
{
    int ex, j;
    double m = frexp(a.hi, &ex), h = 0.0;
    mx_dd v = mx_dd_c(m, ldexp(a.lo, -ex)), t, u, acc;
    e2 += ex;
    if (m < 0.70710678118654752) {
        v.hi *= 2.0;
        v.lo *= 2.0;
        e2--;
    }
    t = mx_dd_div(mx_dd_add(v, mx_dd_c(-1.0, 0.0)), mx_dd_add(v, mx_dd_c(1.0, 0.0)));
    u = mx_dd_mul(t, t);
    for (j = 14; j >= 6; j--)
        h = fma(h, u.hi, MX_LOG_ODD[j]);
    acc = mx_dd_c(h, 0.0);
    for (j = 5; j >= 0; j--)
        acc = mx_dd_add(mx_dd_mul(acc, u), MX_LOG_ODD_DD[j]);
    return mx_dd_add(mx_dd_muld(mx_dd_mul(t, acc), 2.0), mx_dd_muld(MX_LN2_DD, (double)e2));
}

static double mx_bessel_debye(double nu, double x, int kind_k, int scaled)
{
    int sc = 0, xe, j, kk, off = 0;
    double a, b, xm, p, p2, q, qk = 1.0, s = 0.0, ex, kf;
    mx_dd ss, d, g, xp, rr, pref, mm;
    int64_t e;
    if (x > 0x1p500) {
        (void)frexp(x, &sc);
        sc &= ~1;
    }
    a = ldexp(nu, -sc);
    b = ldexp(x, -sc);
    ss = mx_dd_sqrt(mx_dd_add(mx_dd_prod(a, a), mx_dd_prod(b, b)));
    if (x > nu)
        d = mx_dd_div(mx_dd_prod(a, nu), mx_dd_add(ss, mx_dd_c(b, 0.0)));
    else
        d = mx_dd_add(ss, mx_dd_c(-b, 0.0));
    xm = frexp(x, &xe);
    g = mx_dd_add(d, mx_dd_muld(mx_dd_log(mx_dd_div(mx_dd_c(xm, 0.0), mx_dd_add(ss, mx_dd_c(a, 0.0))), (int64_t)xe - sc), nu));
    xp = scaled ? g : mx_dd_add(g, mx_dd_c(x, 0.0));
    if (kind_k)
        xp = mx_dd_neg(xp);
    if (xp.hi > 800.0)
        return DYN_INFINITY;
    if (xp.hi < -800.0)
        return 0.0;
    kf = floor(xp.hi / MX_LN2_DD.hi + 0.5);
    rr = mx_dd_add(xp, mx_dd_neg(mx_dd_muld(MX_LN2_DD, kf)));
    ex = exp(rr.hi);
    p = a / ss.hi;
    p2 = p * p;
    q = (kind_k ? -p : p) / nu;
    for (kk = 1; kk <= MX_DEBYE_TERMS; kk++) {
        double poly = MX_DEBYE_U[off + kk], t;
        for (j = kk - 1; j >= 0; j--)
            poly = fma(poly, p2, MX_DEBYE_U[off + j]);
        qk *= q;
        t = qk * poly;
        s += t;
        off += kk + 1;
        if (fabs(t) < 0x1p-60)
            break;
    }
    if (kind_k)
        pref = mx_dd_sqrt(mx_dd_div(MX_HALF_PI_DD, ss));
    else
        pref = mx_dd_div(mx_dd_c(1.0, 0.0), mx_dd_sqrt(mx_dd_mul(MX_TWO_PI_DD, ss)));
    mm = mx_dd_mul(mx_dd_mul(pref, mx_dd_c(ex, 0.0)), mx_dd_sum(1.0, s));
    e = (int64_t)kf - sc / 2;
    mm = mx_dd_norm(mm, &e);
    return mx_dd_ldexp(mm, e);
}

static double mx_besseli_any(double nu, double x, int scaled)
{
    if (nu <= -1.0) {
        double f = nu - floor(nu);
        if (f < 1.0e-12 || f > 1.0 - 1.0e-12)
            nu = -nu;
        else
            return DYN_NAN;
    }
    if (!(x > 0.0)) {
        if (x == 0.0)
            return (nu == 0.0) ? 1.0 : (nu > 0.0 ? 0.0 : DYN_INFINITY);
        return DYN_NAN;
    }
    if (nu >= MX_DEBYE_NU && nu <= MX_DEBYE_NU_MAX && x < DYN_INFINITY && (nu >= MX_DEBYE_I_SERIES_NU || x > MX_I_SERIES_MAX))
        return mx_bessel_debye(nu, x, 0, scaled);
    if (x <= MX_I_SERIES_MAX
        || fabs(4.0 * nu * nu - 1.0) > MX_I_ASYM_FIRST_TERM_MAX * 8.0 * x)
        return mx_besseli_series(nu, x, scaled);
    return mx_besseli_asym(nu, x, scaled);
}

double dyn_besseli_scaled(double nu, double x)
{
    return mx_besseli_any(nu, x, 1);
}

double dyn_besseli_nu(double nu, double x)
{
    return mx_besseli_any(nu, x, 0);
}


static const double MX_RGAMMA1P[30] = {
    1.0,
    0.5772156649015329,
    -0.6558780715202539,
    -0.04200263503409524,
    0.16653861138229148,
    -0.04219773455554433,
    -0.009621971527876973,
    0.0072189432466631,
    -0.0011651675918590652,
    -0.00021524167411495098,
    0.0001280502823881162,
    -2.013485478078824e-05,
    -1.2504934821426706e-06,
    1.133027231981696e-06,
    -2.056338416977607e-07,
    6.116095104481416e-09,
    5.002007644469223e-09,
    -1.18127457048702e-09,
    1.0434267116911005e-10,
    7.782263439905071e-12,
    -3.696805618642206e-12,
    5.100370287454476e-13,
    -2.0583260535665066e-14,
    -5.348122539423018e-15,
    1.2267786282382608e-15,
    -1.1812593016974588e-16,
    1.1866922547516004e-18,
    1.4123806553180319e-18,
    -2.29874568443537e-19,
    1.7144063219273374e-20,
};

#define MX_K_QUAD_HMAX 0.125
#define MX_K_QUAD_C 0.5

static void mx_besselk_quad2(double mu, double x, mx_dd* k0, mx_dd* k1)
{
    double h = MX_K_QUAD_HMAX, lim = MX_K_QUAD_C / sqrt(x), cutoff = 60.0 + 4.0 * (fabs(mu) + 1.0), m1 = mu + 1.0;
    mx_dd a0 = mx_dd_c(0.5, 0.0), a1 = mx_dd_c(0.5, 0.0);
    int k;
    while (h > lim)
        h *= 0.5;
    for (k = 1; k < 2000000; k++) {
        double t = (double)k * h, ex, e, a, b, ia, ib;
        mx_dd p0, p1;
        if (t < 1.0) {
            double sh = sinh(0.5 * t);
            ex = 2.0 * x * sh * sh;
        } else {
            ex = x * (cosh(t) - 1.0);
        }
        if (ex - m1 * t > cutoff)
            break;
        e = exp(-ex);
        p0 = mx_dd_prod(mu, t);
        p1 = mx_dd_prod(m1, t);
        a = exp(p0.hi);
        a += a * p0.lo;
        b = exp(p1.hi);
        b += b * p1.lo;
        ia = 1.0 / a;
        ib = 1.0 / b;
        a0 = mx_dd_add(a0, mx_dd_prod(0.5 * e, a + ia));
        a1 = mx_dd_add(a1, mx_dd_prod(0.5 * e, b + ib));
    }
    *k0 = mx_dd_muld(a0, h);
    *k1 = mx_dd_muld(a1, h);
}

static void mx_acc(double* s, double* e, double v)
{
    double ns = *s + v, bb = ns - *s;
    *e += (*s - (ns - bb)) + (v - bb);
    *s = ns;
}

static void mx_besselk_temme(double mu, double x, mx_dd* k0, mx_dd* k1)
{
    double x2 = 0.5 * x, d = -log(x2), e = mu * d, pm = pow(x2, -mu), pp = pow(x2, mu), f2;
    mx_dd m2 = mx_dd_prod(mu, mu), g1 = mx_dd_c(0.0, 0.0), g2 = mx_dd_c(0.0, 0.0);
    mx_dd gpl, gmi, fact, ff, p, q, sum, sum1, dq, ch;
    double h1 = 0.0, h2 = 0.0;
    int k;
    for (k = 29; k >= 1; k -= 2)
        h1 = fma(h1, m2.hi, MX_RGAMMA1P[k]);
    for (k = 28; k >= 0; k -= 2)
        h2 = fma(h2, m2.hi, MX_RGAMMA1P[k]);
    g1 = mx_dd_c(h1, 0.0);
    g2 = mx_dd_c(h2, 0.0);
    gpl = mx_dd_add(g2, mx_dd_muld(g1, mu));
    gmi = mx_dd_add(g2, mx_dd_muld(g1, -mu));
    if (mu == 0.0) {
        fact = mx_dd_c(1.0, 0.0);
    } else {
        mx_dd pimu = mx_dd_muld(mx_dd_c(3.141592653589793, 1.2246467991473532e-16), mu);
        fact = mx_dd_divd(pimu, sin(pimu.hi));
    }
    if (fabs(e) < 0.5) {
        double e2 = e * e, t = 1.0;
        int j;
        f2 = 1.0;
        for (j = 1; j < 12; j++) {
            t *= e2 / ((2.0 * j) * (2.0 * j + 1.0));
            f2 += t;
        }
    } else {
        f2 = (pm - pp) / (2.0 * e);
    }
    ch = mx_dd_muld(mx_dd_sum(pm, pp), 0.5);
    ff = mx_dd_mul(fact, mx_dd_add(mx_dd_neg(mx_dd_mul(g1, ch)), mx_dd_muld(mx_dd_muld(g2, f2), d)));
    sum = ff;
    p = mx_dd_div(mx_dd_c(0.5 * pm, 0.0), gpl);
    q = mx_dd_div(mx_dd_c(0.5 * pp, 0.0), gmi);
    dq = mx_dd_prod(x2, x2);
    sum1 = p;
    {
        double ffd = ff.hi, pd = p.hi, qd = q.hi, cd = 1.0, s0 = sum.hi, s1 = sum1.hi, m2d = m2.hi, dqd = dq.hi;
        double e0 = sum.lo, e1 = sum1.lo;
        for (k = 1; k < 500; k++) {
            double i = (double)k, del, del1;
            ffd = (i * ffd + pd + qd) / (i * i - m2d);
            cd *= dqd / i;
            pd /= (i - mu);
            qd /= (i + mu);
            del = cd * ffd;
            mx_acc(&s0, &e0, del);
            del1 = cd * (pd - i * ffd);
            mx_acc(&s1, &e1, del1);
            if (fabs(del) < fabs(s0) * 1.0e-17 && fabs(del1) < fabs(s1) * 1.0e-17)
                break;
        }
        sum = mx_dd_fast(s0, e0);
        sum1 = mx_dd_fast(s1, e1);
    }
    *k0 = sum;
    *k1 = mx_dd_divd(mx_dd_muld(sum1, 2.0), x);
}

#define MX_K_TEMME_MAX 1.0

static double mx_besselk_any(double nu, double x, int scaled)
{
    double n, mu, j;
    mx_dd ka, kb, two_x;
    int64_t e = 0;
    if (isnan(nu) || isnan(x))
        return DYN_NAN;
    if (!(x > 0.0))
        return (x == 0.0) ? DYN_INFINITY : DYN_NAN;
    if (x == DYN_INFINITY)
        return 0.0;
    if (fabs(nu) > 9.0e15 || isinf(nu))
        return DYN_INFINITY;
    if (nu < 0.0)
        nu = -nu;
    if (nu > 1.0e6)
        return DYN_INFINITY;
    if (!scaled && x > 1.0e10)
        return 0.0;
    if (nu >= MX_DEBYE_NU)
        return mx_bessel_debye(nu, x, 1, scaled);
    n = floor(nu + 0.5);
    mu = nu - n;
    if (x <= MX_K_TEMME_MAX) {
        mx_dd ex = mx_dd_c(exp(x), 0.0);
        mx_besselk_temme(mu, x, &ka, &kb);
        if (isinf(kb.hi) || isinf(ka.hi) || isnan(ka.hi))
            return DYN_INFINITY;
        if (scaled) {
            ka = mx_dd_mul(ka, ex);
            kb = mx_dd_mul(kb, ex);
        }
        scaled = 1;
    } else {
        mx_besselk_quad2(mu, x, &ka, &kb);
    }
    if (n == 0.0)
        kb = ka;
    if (isinf(kb.hi) || isinf(ka.hi))
        return DYN_INFINITY;
    two_x = mx_dd_divd(mx_dd_c(2.0, 0.0), x);
    for (j = 1.0; j < n; j += 1.0) {
        mx_dd kc = mx_dd_add(ka, mx_dd_mul(mx_dd_mul(two_x, mx_dd_sum(mu, j)), kb));
        if (!isfinite(kc.hi))
            return DYN_INFINITY;
        ka = kb;
        kb = kc;
        if (kb.hi > 0x1p600) {
            ka.hi = ldexp(ka.hi, -600);
            ka.lo = ldexp(ka.lo, -600);
            kb.hi = ldexp(kb.hi, -600);
            kb.lo = ldexp(kb.lo, -600);
            e += 600;
        }
    }
    if (isinf(kb.hi))
        return DYN_INFINITY;
    if (!scaled) {
        double kk = floor(x / 0.6931471805599453), rr;
        rr = fma(-kk, 0.6931471805599453, x);
        rr = fma(-kk, 2.3190468138462996e-17, rr);
        kb = mx_dd_muld(kb, exp(-rr));
        e -= (int64_t)kk;
    }
    kb = mx_dd_norm(kb, &e);
    return mx_dd_ldexp(kb, e);
}

double dyn_besselk_scaled(double nu, double x)
{
    return mx_besselk_any(nu, x, 1);
}

double dyn_besselk_nu(double nu, double x)
{
    return mx_besselk_any(nu, x, 0);
}

double dyn_besseli(int n, double x)
{
    if (n < 0)
        n = -n;
    if (x < 0.0) {
        double v = dyn_besseli_nu((double)n, -x);
        return (n % 2) ? -v : v;
    }
    return dyn_besseli_nu((double)n, x);
}

double dyn_besselk(int n, double x)
{
    if (n < 0)
        n = -n;
    return dyn_besselk_nu((double)n, x);
}

static unsigned int bessel_order_abs(int n, int* neg)
{
    unsigned int m = (n < 0) ? (unsigned int)(-(long long)n) : (unsigned int)n;
    *neg = (n < 0) && (m & 1u);
    if (m > (unsigned int)INT_MAX)
        m = (unsigned int)INT_MAX;
    return m;
}

static int bessel_underflows(unsigned int m, double x)
{
    double ax = fabs(x);
    if (m < 2u)
        return 0;
    if (ax == 0.0)
        return 1;
    return (double)m * log(ax * 0.5) - mx_lgamma((double)m + 1.0) < -745.0;
}

#define DYN_BESSEL_MAX_ORDER (1u << 24)

int dyn_bessel_order_ok(int n, double x)
{
    int neg;
    unsigned int m = bessel_order_abs(n, &neg);
    if (m <= DYN_BESSEL_MAX_ORDER)
        return 1;
    return bessel_underflows(m, x);
}

double dyn_besselj(int n, double x)
{
    int neg;
    unsigned int m = bessel_order_abs(n, &neg);
    double v;

    if (m == 0)
        return j0(x);
    if (bessel_underflows(m, x))
        return neg ? -0.0 : 0.0;
    if (m > DYN_BESSEL_MAX_ORDER)
        return DYN_NAN;
    if (m == 1)
        v = j1(x);
    else
        v = jn((int)m, x);
    return neg ? -v : v;
}

double dyn_bessely(int n, double x)
{
    int neg;
    unsigned int m = bessel_order_abs(n, &neg);
    double v;

    if (m == 0)
        return y0(x);
    if (m > DYN_BESSEL_MAX_ORDER)
        return DYN_NAN;
    if (m == 1)
        v = y1(x);
    else
        v = yn((int)m, x);
    return neg ? -v : v;
}

int dyn_besselh(int kind, int n, double x, double* re, double* im)
{
    double jv, yv;
    if (kind != 1 && kind != 2)
        return -1;
    jv = dyn_besselj(n, x);
    yv = dyn_bessely(n, x);
    if (re)
        *re = jv;
    if (im)
        *im = (kind == 1) ? yv : -yv;
    return 0;
}

double dyn_ellipk(double m)
{
    double a, b, t;
    int i;

    if (!(m <= 1.0))
        return DYN_NAN;
    if (m == 1.0)
        return DYN_INFINITY;

    a = 1.0;
    b = sqrt(1.0 - m);
    for (i = 0; i < 30; i++) {
        t = 0.5 * (a + b);
        b = sqrt(a * b);
        a = t;
        if (fabs(a - b) < a * 1.0e-17)
            break;
    }
    return M_PI / (2.0 * a);
}

double dyn_ellipe(double m)
{
    double a, b, c, t, sum, pw;
    int i;

    if (!(m <= 1.0))
        return DYN_NAN;
    if (m == 1.0)
        return 1.0;

    a = 1.0;
    b = sqrt(1.0 - m);
    sum = 0.5 * m;
    pw = 1.0;
    for (i = 0; i < 30; i++) {
        t = 0.5 * (a + b);
        c = 0.5 * (a - b);
        b = sqrt(a * b);
        a = t;
        sum += pw * c * c;
        pw *= 2.0;
        if (fabs(c) < 1.0e-18)
            break;
    }
    return (M_PI / (2.0 * a)) * (1.0 - sum);
}

int dyn_ellipj(double u, double m, double* sn, double* cn, double* dn)
{
    double a[32], c[32], t, phi;
    int i, n;

    if (!(m >= 0.0) || !(m <= 1.0))
        return -1;

    if (m < 1.0e-14) {
        double s = sin(u), co = cos(u);
        if (sn)
            *sn = s;
        if (cn)
            *cn = co;
        if (dn)
            *dn = 1.0 - 0.5 * m * s * s;
        return 0;
    }
    if (1.0 - m < 1.0e-14) {
        double th = tanh(u), ch = cosh(u);
        if (sn)
            *sn = th;
        if (cn)
            *cn = 1.0 / ch;
        if (dn)
            *dn = 1.0 / ch;
        return 0;
    }

    a[0] = 1.0;
    c[0] = sqrt(m);
    {
        double b = sqrt(1.0 - m);
        n = 0;
        while (fabs(c[n]) > 1.0e-17 * fabs(a[n]) && n < 30) {
            t = 0.5 * (a[n] + b);
            c[n + 1] = 0.5 * (a[n] - b);
            b = sqrt(a[n] * b);
            n++;
            a[n] = t;
        }
    }

    phi = ldexp(a[n] * u, n);
    for (i = n; i > 0; i--) {
        double s = c[i] / a[i] * sin(phi);
        if (s > 1.0)
            s = 1.0;
        if (s < -1.0)
            s = -1.0;
        phi = 0.5 * (asin(s) + phi);
    }

    {
        double s = sin(phi), co = cos(phi);
        if (sn)
            *sn = s;
        if (cn)
            *cn = co;
        if (dn)
            *dn = sqrt(1.0 - m * s * s);
    }
    return 0;
}

double dyn_legendre(int n, int m, double x)
{
    double pmm, pmmp1, pll, somx2, fact;
    int i, ll;

    if (m < 0 || n < 0 || m > n || fabs(x) > 1.0)
        return DYN_NAN;

    pmm = 1.0;
    if (m > 0) {
        somx2 = sqrt((1.0 - x) * (1.0 + x));
        fact = 1.0;
        for (i = 1; i <= m; i++) {
            pmm *= -fact * somx2;
            fact += 2.0;
        }
    }
    if (n == m)
        return pmm;

    pmmp1 = x * (2.0 * m + 1.0) * pmm;
    if (n == m + 1)
        return pmmp1;

    pll = 0.0;
    for (ll = m + 2; ll <= n; ll++) {
        pll = (x * (2.0 * ll - 1.0) * pmmp1 - (ll + m - 1.0) * pmm) / (double)(ll - m);
        pmm = pmmp1;
        pmmp1 = pll;
    }
    return pll;
}

static const double MX_B2K[] = {
    1.0 / 6.0, -1.0 / 30.0, 1.0 / 42.0,
    -1.0 / 30.0, 5.0 / 66.0, -691.0 / 2730.0,
    7.0 / 6.0, -3617.0 / 510.0, 43867.0 / 798.0,
    -174611.0 / 330.0
};

static const double MX_DIGAMMA_ROOT_HI = 1.4616321449683622;
static const double MX_DIGAMMA_ROOT_LO = 9.549995429965697e-17;
static const double MX_DIGAMMA_AT_ROOT[] = {
    0.9676722454476212, -0.4427631689835921, 0.258499760955651,
    -0.16394270544240652, 0.10782405069126237, -0.07219956125645471,
    0.04880428816414311, -0.03316112647484736, 0.022597648232218104,
    -0.01542476590494896, 0.010538791616612175, -0.007204534386356869,
    0.004926781395729853, -0.003369801655439328, 0.002305126326734928,
    -0.0015769367714301972, 0.0010788252019162967, -0.0007380709389960052,
    0.000504953265834602, -0.0003454680251063077, 0.00023635601564027053,
    -0.00016170622091974803, 0.0001106337276874741, -7.569179582195066e-05,
    5.178575795222081e-05
};

static double mx_cotpi(double x)
{
    double r = x - nearbyint(x);
    return sin(M_PI * (0.5 - fabs(r))) / sin(M_PI * r);
}

double dyn_digamma(double x)
{
    double r = 0.0, f, x2;

    if (isnan(x))
        return DYN_NAN;
    if (x == 0.0)
        return copysign(DYN_INFINITY, -x);
    if (x < 0.0 && x == floor(x))
        return DYN_NAN;
    if (x < 0.5)
        return dyn_digamma(1.0 - x) - M_PI * mx_cotpi(x);
    if (fabs(x - MX_DIGAMMA_ROOT_HI) < 0.25) {
        double t = (x - MX_DIGAMMA_ROOT_HI) - MX_DIGAMMA_ROOT_LO, acc = 0.0;
        int k = (int)(sizeof(MX_DIGAMMA_AT_ROOT) / sizeof(MX_DIGAMMA_AT_ROOT[0]));
        while (k-- > 0)
            acc = acc * t + MX_DIGAMMA_AT_ROOT[k];
        return acc * t;
    }

    while (x < 15.0) {
        r -= 1.0 / x;
        x += 1.0;
    }
    x2 = 1.0 / (x * x);
    f = log(x) - 0.5 / x;
    f -= x2 * (1.0 / 12.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 252.0 - x2 * (1.0 / 240.0 - x2 * (1.0 / 132.0)))));
    return r + f;
}

static double mx_polygamma_reflect(int n, double x)
{
    double p[67], q[67], c = mx_cotpi(x), c2 = c * c, v = 0.0, pin = M_PI;
    int i, k, deg = 1;

    memset(p, 0, sizeof(p));
    p[0] = 0.0;
    p[1] = 1.0;
    for (i = 0; i < n; i++) {
        memset(q, 0, sizeof(q));
        for (k = 1; k <= deg; k++) {
            double d = -(double)k * p[k];
            q[k - 1] += d;
            q[k + 1] += d;
        }
        memcpy(p, q, sizeof(p));
        deg++;
        pin *= M_PI;
    }
    for (k = deg; k >= 0; k -= 2)
        v = v * c2 + p[k];
    if (deg & 1)
        v *= c;
    return ((n & 1) ? -1.0 : 1.0) * dyn_polygamma(n, 1.0 - x) - pin * v;
}

double dyn_polygamma(int n, double x)
{
    double acc = 0.0, sgn, nfact, term, xp, sum;
    int i, k;

    if (n < 0)
        return DYN_NAN;
    if (n == 0)
        return dyn_digamma(x);
    if (x <= 0.0 && x == floor(x))
        return DYN_NAN;
    if (x < 0.0)
        return mx_polygamma_reflect(n, x);

    sgn = ((n + 1) % 2 == 0) ? 1.0 : -1.0;
    nfact = 1.0;
    for (i = 1; i <= n; i++)
        nfact *= (double)i;

    while (x < 15.0) {
        acc += sgn * nfact / pow(x, (double)(n + 1));
        x += 1.0;
    }

    {
        double nm1fact = nfact / (double)n;
        sum = nm1fact / pow(x, (double)n) + nfact / (2.0 * pow(x, (double)(n + 1)));
        term = nfact * (double)(n + 1) / 2.0;
        xp = pow(x, (double)(n + 2));
        for (k = 1; k <= (int)(sizeof(MX_B2K) / sizeof(MX_B2K[0])); k++) {
            double add = MX_B2K[k - 1] * term / xp;
            sum += add;
            if (fabs(add) < fabs(sum) * 1.0e-17)
                break;
            term *= (double)(n + 2 * k) * (double)(n + 2 * k + 1) / ((2.0 * k + 1.0) * (2.0 * k + 2.0));
            xp *= x * x;
        }
        return acc + sgn * sum;
    }
}

static void mx_airy_series(double x, double* ai, double* aip, double* bi,
    double* bip)
{
    const mx_dd C1 = mx_dd_c(0.3550280538878172, 2.05233632436212e-17);
    const mx_dd C2 = mx_dd_c(0.2588194037928068, -2.522243111610832e-17);
    const mx_dd SQRT3 = mx_dd_c(1.7320508075688772, 1.0035084221806903e-16);
    mx_dd x3d = mx_dd_muld(mx_dd_prod(x, x), x);
    double x3 = x3d.hi, x3l = x3d.lo;
    double tf = 1.0, cf = 0.0, tg = x, cg = 0.0;
    double sf = 1.0, ef = 0.0, sg = x, eg = 0.0, sfp = 0.0, efp = 0.0, sgp = 1.0, egp = 0.0;
    mx_dd f, g, fp, gp, a, b;
    int k;

    for (k = 1; k < 200; k++) {
        double kk = (double)k, m = 3.0 * kk, df = (m - 1.0) * m, dg = m * (m + 1.0);
        double iv = 1.0 / (df * dg), idf = dg * iv, idg = df * iv;
        double rf = x3 * idf, rg = x3 * idg, n, u;
        double drf = (fma(-rf, df, x3) + x3l) * idf, drg = (fma(-rg, dg, x3) + x3l) * idg;
        n = tf * rf;
        cf = fma(tf, rf, -n) + cf * rf + tf * drf;
        tf = n;
        n = tg * rg;
        cg = fma(tg, rg, -n) + cg * rg + tg * drg;
        tg = n;
        mx_acc(&sf, &ef, tf);
        ef += cf;
        mx_acc(&sg, &eg, tg);
        eg += cg;
        u = tf * m;
        mx_acc(&sfp, &efp, u);
        efp += fma(tf, m, -u) + cf * m;
        u = tg * (m + 1.0);
        mx_acc(&sgp, &egp, u);
        egp += fma(tg, m + 1.0, -u) + cg * (m + 1.0);
        if (k > 3 && fabs(tf) * m + fabs(tg) * (m + 1.0) < 1.0e-34 * (fabs(sf) + fabs(sg) + fabs(sfp) + fabs(sgp)))
            break;
    }
    f = mx_dd_c(sf, ef);
    g = mx_dd_c(sg, eg);
    fp = mx_dd_c(sfp, efp);
    gp = mx_dd_c(sgp, egp);
    if (x != 0.0) {
        fp = mx_dd_divd(fp, x);
        gp = mx_dd_add(mx_dd_divd(mx_dd_add(gp, mx_dd_c(-1.0, 0.0)), x), mx_dd_c(1.0, 0.0));
    } else {
        fp = mx_dd_c(0.0, 0.0);
        gp = mx_dd_c(1.0, 0.0);
    }
    a = mx_dd_mul(C1, f);
    b = mx_dd_mul(C2, g);
    if (ai) {
        mx_dd v = mx_dd_add(a, mx_dd_neg(b));
        *ai = v.hi + v.lo;
    }
    if (bi) {
        mx_dd v = mx_dd_mul(SQRT3, mx_dd_add(a, b));
        *bi = v.hi + v.lo;
    }
    a = mx_dd_mul(C1, fp);
    b = mx_dd_mul(C2, gp);
    if (aip) {
        mx_dd v = mx_dd_add(a, mx_dd_neg(b));
        *aip = v.hi + v.lo;
    }
    if (bip) {
        mx_dd v = mx_dd_mul(SQRT3, mx_dd_add(a, b));
        *bip = v.hi + v.lo;
    }
}

static mx_dd mx_airy_zeta(double z)
{
    double s = sqrt(z);
    mx_dd sd = mx_dd_fast(s, fma(-s, s, z) / (2.0 * s));
    return mx_dd_mul(mx_dd_muld(sd, z), mx_dd_c(0.6666666666666666, 3.700743415417188e-17));
}

static void mx_airy_asym_neg(double x, double* ai, double* aip, double* bi,
    double* bip)
{
    double z = -x;
    mx_dd zd = mx_airy_zeta(z), th;
    double zeta = zd.hi;
    double u_even = 1.0, u_odd = 0.0;
    double v_even = 1.0, v_odd = 0.0;
    double u = 1.0, zp = 1.0, prev = HUGE_VAL;
    int k;

    for (k = 1; k < 30; k++) {
        double kk = (double)k, t, v, sgn;
        u *= ((6.0 * kk - 5.0) * (6.0 * kk - 3.0) * (6.0 * kk - 1.0)) / ((2.0 * kk - 1.0) * 216.0 * kk);
        zp *= zeta;
        t = u / zp;
        if (fabs(t) > prev)
            break;
        prev = fabs(t);
        sgn = ((k / 2) % 2 == 0) ? 1.0 : -1.0;
        v = t * (6.0 * kk + 1.0) / (1.0 - 6.0 * kk);
        if (k % 2 == 0) {
            u_even += sgn * t;
            v_even += sgn * v;
        } else {
            u_odd += sgn * t;
            v_odd += sgn * v;
        }
        if (fabs(t) < 1.0e-19)
            break;
    }

    th = mx_dd_add(zd, mx_dd_c(-0.7853981633974483, -3.061616997868383e-17));
    {
        double c0 = cos(th.hi), s0 = sin(th.hi);
        double c = c0 - s0 * th.lo, s = s0 + c0 * th.lo;
        double q = pow(z, 0.25);
        double p1 = 0.5641895835477563 / q;
        double p2 = 0.5641895835477563 * q;
        if (ai)
            *ai = p1 * (c * u_even + s * u_odd);
        if (bi)
            *bi = p1 * (-s * u_even + c * u_odd);
        if (aip)
            *aip = p2 * (s * v_even - c * v_odd);
        if (bip)
            *bip = p2 * (c * v_even + s * v_odd);
    }
}

static double mx_dd_out(mx_dd a, mx_dd b, mx_dd c, double d)
{
    mx_dd v = mx_dd_muld(mx_dd_mul(mx_dd_mul(a, b), c), d);
    return v.hi + v.lo;
}

static void mx_airy_asym_pos(double x, double* ai, double* aip, double* bi,
    double* bip)
{
    mx_dd zd = mx_airy_zeta(x), q, iq, em, ep;
    mx_dd sa = mx_dd_c(1.0, 0.0), sap = sa, sb = sa, sbp = sa;
    const mx_dd H = mx_dd_c(0.28209479177387814, 3.83386490329147e-18);
    const mx_dd S = mx_dd_c(0.5641895835477563, 7.66772980658294e-18);
    double zeta = zd.hi, u = 1.0, zp = 1.0, prev = HUGE_VAL, e1, e2;
    int k;

    for (k = 1; k < 40; k++) {
        double kk = (double)k, t, v, sg = (k % 2) ? -1.0 : 1.0;
        u *= ((6.0 * kk - 5.0) * (6.0 * kk - 3.0) * (6.0 * kk - 1.0)) / ((2.0 * kk - 1.0) * 216.0 * kk);
        zp *= zeta;
        t = u / zp;
        if (fabs(t) > prev)
            break;
        prev = fabs(t);
        v = t * (6.0 * kk + 1.0) / (1.0 - 6.0 * kk);
        sa = mx_dd_add(sa, mx_dd_c(sg * t, 0.0));
        sap = mx_dd_add(sap, mx_dd_c(sg * v, 0.0));
        sb = mx_dd_add(sb, mx_dd_c(t, 0.0));
        sbp = mx_dd_add(sbp, mx_dd_c(v, 0.0));
        if (fabs(t) < 1.0e-19)
            break;
    }
    q = mx_dd_sqrt(mx_dd_sqrt(mx_dd_c(x, 0.0)));
    iq = mx_dd_div(mx_dd_c(1.0, 0.0), q);
    e1 = exp(-zeta);
    e2 = exp(zeta);
    em = mx_dd_sum(1.0, -zd.lo);
    ep = mx_dd_sum(1.0, zd.lo);
    if (ai)
        *ai = mx_dd_out(H, iq, mx_dd_mul(sa, em), e1);
    if (aip)
        *aip = -mx_dd_out(H, q, mx_dd_mul(sap, em), e1);
    if (bi)
        *bi = mx_dd_out(S, iq, mx_dd_mul(sb, ep), e2);
    if (bip)
        *bip = mx_dd_out(S, q, mx_dd_mul(sbp, ep), e2);
}

static void mx_airy_mid(double x, double* ai, double* aip, double* bi,
    double* bip)
{
    mx_dd zd = mx_airy_zeta(x), r3, em;
    const mx_dd INVPI = mx_dd_c(0.3183098861837907, -1.9678676675182486e-17);
    const mx_dd INVSQRT3 = mx_dd_c(0.5773502691896257, 3.3450280739356345e-17);
    double zeta = zd.hi, e1 = exp(-zeta);
    r3 = mx_dd_sqrt(mx_dd_divd(mx_dd_c(x, 0.0), 3.0));
    em = mx_dd_sum(1.0, -zd.lo);
    if (ai || aip) {
        mx_dd k13, k43, k23;
        mx_besselk_quad2(1.0 / 3.0, zeta, &k13, &k43);
        k23 = mx_dd_add(k43, mx_dd_neg(mx_dd_mul(k13, mx_dd_divd(mx_dd_c(2.0, 0.0), 3.0 * zeta))));
        if (ai)
            *ai = mx_dd_out(INVPI, r3, mx_dd_mul(em, k13), e1);
        if (aip)
            *aip = -mx_dd_out(INVPI, mx_dd_muld(INVSQRT3, x), mx_dd_mul(em, k23), e1);
    }
    if (bi || bip)
        mx_airy_series(x, NULL, NULL, bi, bip);
}

#define MX_AIRY_SERIES_LO (-12.0)
#define MX_AIRY_SERIES_HI 8.0
#define MX_AIRY_ASYM_POS 10.0

void dyn_airy(double x, double* ai, double* aip, double* bi, double* bip)
{
    if (x >= MX_AIRY_ASYM_POS) {
        mx_airy_asym_pos(x, ai, aip, bi, bip);
        return;
    }
    if (x > MX_AIRY_SERIES_HI) {
        mx_airy_mid(x, ai, aip, bi, bip);
        return;
    }
    if (x >= MX_AIRY_SERIES_LO) {
        mx_airy_series(x, ai, aip, bi, bip);
        return;
    }
    mx_airy_asym_neg(x, ai, aip, bi, bip);
}

double dyn_erfinv(double y)
{
    double w, p, x;
    if (isnan(y))
        return DYN_NAN;
    if (y >= 1.0)
        return (y == 1.0) ? DYN_INFINITY : DYN_NAN;
    if (y <= -1.0)
        return (y == -1.0) ? -DYN_INFINITY : DYN_NAN;
    w = -log((1.0 - y) * (1.0 + y));
    if (w < 6.25) {
        w -= 3.125;
        p = -3.6444120640178196996e-21;
        p = -1.685059138182016589e-19 + p * w;
        p = 1.2858480715256400167e-18 + p * w;
        p = 1.115787767802518096e-17 + p * w;
        p = -1.333171662854620906e-16 + p * w;
        p = 2.0972767875968561637e-17 + p * w;
        p = 6.6376381343583238325e-15 + p * w;
        p = -4.0545662729752068639e-14 + p * w;
        p = -8.1519341976054721522e-14 + p * w;
        p = 2.6335093153082322977e-12 + p * w;
        p = -1.2975133253453532498e-11 + p * w;
        p = -5.4154120542946279317e-11 + p * w;
        p = 1.051212273321532285e-09 + p * w;
        p = -4.1126339803469836976e-09 + p * w;
        p = -2.9070369957882005086e-08 + p * w;
        p = 4.2347877827932403518e-07 + p * w;
        p = -1.3654692000834678645e-06 + p * w;
        p = -1.3882523362786468719e-05 + p * w;
        p = 0.0001867342080340571352 + p * w;
        p = -0.00074070253416626697512 + p * w;
        p = -0.0060336708714301490533 + p * w;
        p = 0.24015818242558961693 + p * w;
        p = 1.6536545626831027356 + p * w;
    } else if (w < 16.0) {
        w = sqrt(w) - 3.25;
        p = 2.2137376921775787049e-09;
        p = 9.0756561938885390979e-08 + p * w;
        p = -2.7517406297064545428e-07 + p * w;
        p = 1.8239629214389227755e-08 + p * w;
        p = 1.5027403968909827627e-06 + p * w;
        p = -4.013867526981545969e-06 + p * w;
        p = 2.9234449089955446044e-06 + p * w;
        p = 1.2475304481671778723e-05 + p * w;
        p = -4.7318229009055733981e-05 + p * w;
        p = 6.8284851459573175448e-05 + p * w;
        p = 2.4031110387097893999e-05 + p * w;
        p = -0.0003550375203628474796 + p * w;
        p = 0.00095328937973738049703 + p * w;
        p = -0.0016882755560235047313 + p * w;
        p = 0.0024914420961078508066 + p * w;
        p = -0.0037512085075692412107 + p * w;
        p = 0.005370914553590063617 + p * w;
        p = 1.0052589676941592334 + p * w;
        p = 3.0838856104922207635 + p * w;
    } else {
        w = sqrt(w) - 5.0;
        p = -2.7109920616438573243e-11;
        p = -2.5556418169965252055e-10 + p * w;
        p = 1.5076572693500548083e-09 + p * w;
        p = -3.7894654401267369937e-09 + p * w;
        p = 7.6157012080783393804e-09 + p * w;
        p = -1.4960026627149240478e-08 + p * w;
        p = 2.9147953450901080826e-08 + p * w;
        p = -6.7711997758452339498e-08 + p * w;
        p = 2.2900482228026654717e-07 + p * w;
        p = -9.9298272942317002539e-07 + p * w;
        p = 4.5260625972231537039e-06 + p * w;
        p = -1.9681778105531670567e-05 + p * w;
        p = 7.5995277030017761139e-05 + p * w;
        p = -0.00021503011930044477347 + p * w;
        p = -0.00013871931833623122026 + p * w;
        p = 1.0103004648645343977 + p * w;
        p = 4.8499064014085844221 + p * w;
    }
    x = p * y;
    for (int i = 0; i < 2; i++) {
        double e = erf(x) - y;
        double d = 1.1283791670955125739 * exp(-x * x);
        if (d == 0.0 || isnan(d))
            break;
        x -= e / d;
    }
    return x;
}

double dyn_erfcinv(double y)
{
    double t, x;
    int i;
    if (!(y >= 0.0) || !(y <= 2.0))
        return DYN_NAN;
    if (y >= 0.25 && y <= 1.75)
        return dyn_erfinv(1.0 - y);
    if (y > 1.75)
        return -dyn_erfcinv(2.0 - y);
    if (y == 0.0)
        return DYN_INFINITY;
    t = sqrt(-2.0 * log(0.5 * y));
    x = (t - (2.515517 + t * (0.802853 + t * 0.010328)) / (1.0 + t * (1.432788 + t * (0.189269 + t * 0.001308))))
        * 0.70710678118654752440;
    for (i = 0; i < 12; i++) {
        double f = erfc(x) - y;
        double d = -1.1283791670955125739 * exp(-x * x);
        double dx = f / (d + x * f);
        x -= dx;
        if (fabs(dx) <= 1.0e-16 * fabs(x))
            break;
    }
    return x;
}
