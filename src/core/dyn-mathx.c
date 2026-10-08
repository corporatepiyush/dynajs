#include "dyn-mathx.h"
#include "dyn-cdefs.h"

#include <float.h>
#include <limits.h>
#define _REENTRANT
#include <math.h>

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

static double mx_lgamma(double x)
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
    return sum * exp(-x + a * log(x) - mx_lgamma(a));
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
    return exp(-x + a * log(x) - mx_lgamma(a)) * h;
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
        err = dyn_gammainc_p(a, x) - p;
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

    bt = exp(mx_lgamma(a + b) - mx_lgamma(a) - mx_lgamma(b) + a * log(x) + b * log1p(-x));
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

static double mx_besseli_series(double nu, double x, int scaled)
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
        return scaled ? pref * sum : exp(x) * pref * sum;
    }
}

double dyn_besseli_scaled(double nu, double x)
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
    if (x <= MX_I_SERIES_MAX
        || fabs(4.0 * nu * nu - 1.0) > MX_I_ASYM_FIRST_TERM_MAX * 8.0 * x)
        return mx_besseli_series(nu, x, 1);
    return mx_besseli_asym(nu, x, 1);
}

double dyn_besseli_nu(double nu, double x)
{
    double s = dyn_besseli_scaled(nu, x);
    return s * exp(x);
}

static double mx_besselk_quad(double nu, double x)
{
    double h, sum, t, f, cutoff;
    int k;

    h = 0.25 / sqrt(x);
    if (h > 0.06)
        h = 0.06;
    cutoff = 60.0 + fabs(nu) * 4.0;

    sum = 0.5;
    for (k = 1; k < 200000; k++) {
        double arg;
        t = (double)k * h;
        arg = x * (cosh(t) - 1.0) - nu * t;
        if (arg > cutoff)
            break;
        f = exp(-x * (cosh(t) - 1.0)) * cosh(nu * t);
        sum += f;
        if (!isfinite(sum))
            break;
        if (f < 1.0e-20 && t > 1.0)
            break;
    }
    return sum * h;
}

static double mx_k_connection(double nu, double x)
{
    double im = mx_besseli_series(-nu, x, 0);
    double ip = mx_besseli_series(nu, x, 0);
    return M_PI * 0.5 * (im - ip) / sin(nu * M_PI);
}

static double mx_besselk_small_int(int n, double x)
{
    double half = 0.5 * x, hh = half * half;
    double sum, psi1, psi2, logh, res;
    int k, j;
    const double EULER = 0.57721566490153286061;

    logh = log(half);

    if (n == 0) {
        double t = 1.0, s = -EULER, harm = 0.0;
        sum = s;
        for (k = 1; k < 200; k++) {
            t *= hh / ((double)k * (double)k);
            harm += 1.0 / (double)k;
            s = harm - EULER;
            sum += t * s;
            if (t * fabs(s) < fabs(sum) * 1.0e-18)
                break;
        }
        return -logh * mx_besseli_series(0.0, x, 0) + sum;
    }

    res = 0.0;
    {
        double pw = pow(half, -(double)n) * 0.5;
        double fact_nk1 = 1.0;
        double kfact = 1.0;
        double sgn_pow = 1.0;
        for (j = 1; j < n; j++)
            fact_nk1 *= (double)j;
        for (k = 0; k < n; k++) {
            if (k > 0) {
                kfact *= (double)k;
                fact_nk1 /= (double)(n - k);
                sgn_pow *= -hh;
            }
            res += pw * fact_nk1 / kfact * sgn_pow;
        }
    }
    res += ((n % 2) ? 1.0 : -1.0) * logh * mx_besseli_series((double)n, x, 0);

    {
        double sgn = ((n % 2) ? -1.0 : 1.0) * 0.5 * pow(half, (double)n);
        double kfact = 1.0, nkfact = 1.0, pw = 1.0;
        for (j = 1; j <= n; j++)
            nkfact *= (double)j;
        psi1 = -EULER;
        psi2 = -EULER;
        for (j = 1; j <= n; j++)
            psi2 += 1.0 / (double)j;
        sum = 0.0;
        for (k = 0; k < 300; k++) {
            double add;
            if (k > 0) {
                kfact *= (double)k;
                nkfact *= (double)(n + k);
                pw *= hh;
                psi1 += 1.0 / (double)k;
                psi2 += 1.0 / (double)(n + k);
            }
            add = (psi1 + psi2) * pw / (kfact * nkfact);
            sum += add;
            if (fabs(add) < fabs(sum) * 1.0e-18 && k > 2)
                break;
        }
        res += sgn * sum;
    }
    return res;
}

static double mx_besselk_small(double nu, double x)
{
    double nu0, ka, kb, kc;
    double frac = nu - floor(nu);
    double steps, j;

    if (frac < 1.0e-12 || frac > 1.0 - 1.0e-12) {
        nu0 = 0.0;
        steps = floor(nu + 0.5);
        ka = mx_besselk_small_int(0, x);
        kb = mx_besselk_small_int(1, x);
    } else {
        nu0 = frac;
        steps = floor(nu);
        ka = mx_k_connection(nu0, x);
        kb = mx_k_connection(1.0 - nu0, x) + (2.0 * nu0 / x) * ka;
    }

    if (steps <= 0.0)
        return ka;
    for (j = 1.0; j <= steps - 1.0; j += 1.0) {
        kc = ka + (2.0 * (nu0 + j) / x) * kb;
        if (!isfinite(kc))
            return kc;
        ka = kb;
        kb = kc;
    }
    return kb;
}

double dyn_besselk_scaled(double nu, double x)
{
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

    if (x >= 0.5)
        return mx_besselk_quad(nu, x);
    if (nu > 1.0e6)
        return DYN_INFINITY;
    return mx_besselk_small(nu, x) * exp(x);
}

double dyn_besselk_nu(double nu, double x)
{
    double s = dyn_besselk_scaled(nu, x);
    return s * exp(-x);
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
    c = sqrt(m);
    sum = 0.5 * c * c;
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
        return dyn_digamma(1.0 - x) - M_PI / tan(M_PI * x);

    while (x < 15.0) {
        r -= 1.0 / x;
        x += 1.0;
    }
    x2 = 1.0 / (x * x);
    f = log(x) - 0.5 / x;
    f -= x2 * (1.0 / 12.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 252.0 - x2 * (1.0 / 240.0 - x2 * (1.0 / 132.0)))));
    return r + f;
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
    if (x < 0.0) {
        return DYN_NAN;
    }

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
    const double C1 = 0.355028053887817239260;
    const double C2 = 0.258819403792806798405;
    const double SQRT3 = 1.732050807568877293527;
    double f = 1.0, g = x, fp = 0.0, gp = 1.0;
    double tf = 1.0, tg = x;
    double x3 = x * x * x;
    int k;

    for (k = 1; k < 200; k++) {
        double kk = (double)k;
        tf *= x3 / ((3.0 * kk - 1.0) * (3.0 * kk));
        tg *= x3 / ((3.0 * kk) * (3.0 * kk + 1.0));
        f += tf;
        g += tg;
        if (x != 0.0) {
            fp += tf * (3.0 * kk) / x;
            gp += tg * (3.0 * kk + 1.0) / x;
        }
        if (fabs(tf) + fabs(tg) < (fabs(f) + fabs(g)) * 1.0e-19 && k > 3)
            break;
    }

    if (ai)
        *ai = C1 * f - C2 * g;
    if (bi)
        *bi = SQRT3 * (C1 * f + C2 * g);
    if (aip)
        *aip = C1 * fp - C2 * gp;
    if (bip)
        *bip = SQRT3 * (C1 * fp + C2 * gp);
}

static void mx_airy_asym_neg(double x, double* ai, double* aip, double* bi,
    double* bip)
{
    double z = -x, zeta = (2.0 / 3.0) * pow(z, 1.5);
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

    {
        double c = cos(zeta - M_PI / 4.0), s = sin(zeta - M_PI / 4.0);
        double p1 = 1.0 / (sqrt(M_PI) * pow(z, 0.25));
        double p2 = pow(z, 0.25) / sqrt(M_PI);
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

void dyn_airy(double x, double* ai, double* aip, double* bi, double* bip)
{
    if (x >= 0.1) {
        double zeta = (2.0 / 3.0) * pow(x, 1.5);
        const double SQRT3 = 1.732050807568877293527;
        if (ai)
            *ai = (1.0 / M_PI) * sqrt(x / 3.0) * dyn_besselk_nu(1.0 / 3.0, zeta);
        if (aip)
            *aip = -(x / (M_PI * SQRT3)) * dyn_besselk_nu(2.0 / 3.0, zeta);
        if (bi)
            *bi = sqrt(x / 3.0) * (dyn_besseli_nu(-1.0 / 3.0, zeta) + dyn_besseli_nu(1.0 / 3.0, zeta));
        if (bip)
            *bip = (x / SQRT3) * (dyn_besseli_nu(-2.0 / 3.0, zeta) + dyn_besseli_nu(2.0 / 3.0, zeta));
        return;
    }
    if (x >= -7.0) {
        mx_airy_series(x, ai, aip, bi, bip);
        return;
    }
    mx_airy_asym_neg(x, ai, aip, bi, bip);
}
