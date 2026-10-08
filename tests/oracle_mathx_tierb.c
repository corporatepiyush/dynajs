#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/core/dyn-mathx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    const char *name;
    double worst;
    double at;
    long long n;
} regime;

static regime R[64];
static int n_regimes;

static regime *reg(const char *name)
{
    int i;
    for (i = 0; i < n_regimes; i++)
        if (strcmp(R[i].name, name) == 0)
            return &R[i];
    R[n_regimes].name = name;
    R[n_regimes].worst = 0.0;
    R[n_regimes].at = 0.0;
    R[n_regimes].n = 0;
    return &R[n_regimes++];
}

static void note(const char *name, double got, double want, double at)
{
    regime *r = reg(name);
    double e, scale = fabs(want);
    if (scale < 1.0e-280)
        e = fabs(got - want);
    else
        e = fabs(got - want) / scale;
    r->n++;
    if (!(e <= r->worst)) {
        r->worst = e;
        r->at = at;
    }
}

static void note_scaled(const char *name, double got, double want, double scale,
                        double at)
{
    regime *r = reg(name);
    double e = fabs(got - want) / (fabs(scale) > 1.0e-280 ? fabs(scale) : 1.0);
    r->n++;
    if (!(e <= r->worst)) {
        r->worst = e;
        r->at = at;
    }
}

static void note_seam(const char *name, double (*f)(double, void *), void *ud,
                      double s, double h)
{
    double lo, hi;
    double a = f(s - 3.0 * h, ud), b = f(s - 2.0 * h, ud), c = f(s - h, ud);
    double d = f(s + h, ud), e = f(s + 2.0 * h, ud), g = f(s + 3.0 * h, ud);
    lo = 3.0 * c - 3.0 * b + a;
    hi = 3.0 * d - 3.0 * e + g;
    note(name, lo, hi, s);
}

static void t_gammainc(void)
{
    double a, x;

    for (x = 0.01; x < 40.0; x *= 1.3)
        note("gammainc P(1,x) vs 1-e^-x", dyn_gammainc_p(1.0, x), -expm1(-x), x);

    for (x = 0.01; x < 40.0; x *= 1.3)
        note("gammainc P(1/2,x) vs erf", dyn_gammainc_p(0.5, x), erf(sqrt(x)), x);

    for (a = 0.1; a < 200.0; a *= 1.7)
        for (x = 0.01; x < 400.0; x *= 1.6)
            note("gammainc P+Q=1", dyn_gammainc_p(a, x) + dyn_gammainc_q(a, x),
                 1.0, x);

    for (a = 0.3; a < 60.0; a *= 1.5)
        for (x = 0.05; x < 80.0; x *= 1.7) {
            double pa = dyn_gammainc_p(a, x);
            double corr = exp(a * log(x) - x - lgamma(a + 1.0));
            note_scaled("gammainc recurrence", dyn_gammainc_p(a + 1.0, x),
                        pa - corr, fabs(pa) + fabs(corr), x);
        }

    for (x = 1.0; x < 700.0; x *= 1.4)
        note("gammainc Q tail vs e^-x", dyn_gammainc_q(1.0, x), exp(-x), x);
}

static void t_gammaincinv(void)
{
    double a, p;
    for (a = 0.2; a < 120.0; a *= 1.6)
        for (p = 0.001; p < 0.999; p += 0.037) {
            double x = dyn_gammaincinv(a, p);
            note("gammaincinv round trip", dyn_gammainc_p(a, x), p, a);
        }
}

static double binom_tail(int a, int b, double x)
{
    int n = a + b - 1, j;
    double s = 0.0;
    for (j = a; j <= n; j++) {
        double lc = lgamma(n + 1.0) - lgamma(j + 1.0) - lgamma(n - j + 1.0);
        s += exp(lc + j * log(x) + (n - j) * log1p(-x));
    }
    return s;
}

static void t_betainc(void)
{
    int a, b;
    double x;

    for (x = 0.02; x < 0.99; x += 0.03) {
        note("betainc I_x(1,1)=x", dyn_betainc(x, 1.0, 1.0), x, x);
        for (a = 1; a <= 8; a++)
            for (b = 1; b <= 8; b++) {
                note("betainc vs binomial tail", dyn_betainc(x, a, b),
                     binom_tail(a, b, x), x);
                note("betainc symmetry",
                     dyn_betainc(x, a, b) + dyn_betainc(1.0 - x, b, a), 1.0, x);
            }
    }

    {
        double fa, fb;
        for (fa = 0.3; fa < 30.0; fa *= 1.9)
            for (fb = 0.4; fb < 30.0; fb *= 1.9)
                for (x = 0.05; x < 0.96; x += 0.09)
                    note("betainc symmetry (real a,b)",
                         dyn_betainc(x, fa, fb) + dyn_betainc(1.0 - x, fb, fa),
                         1.0, x);
    }
}

static void t_betaincinv(void)
{
    double a, b, p;
    for (a = 0.4; a < 40.0; a *= 2.1)
        for (b = 0.5; b < 40.0; b *= 2.1)
            for (p = 0.01; p < 0.99; p += 0.047) {
                double x = dyn_betaincinv(p, a, b);
                note("betaincinv round trip", dyn_betainc(x, a, b), p, p);
            }
}

static double f_i0(double x, void *u)  { (void)u; return dyn_besseli_scaled(0.0, x); }
static double f_i13(double x, void *u) { (void)u; return dyn_besseli_scaled(1.0/3.0, x); }
static double f_k0(double x, void *u)  { (void)u; return dyn_besselk_scaled(0.0, x); }
static double f_k13(double x, void *u) { (void)u; return dyn_besselk_scaled(1.0/3.0, x); }
static double f_k43(double x, void *u) { (void)u; return dyn_besselk_scaled(4.0/3.0, x); }

static void t_bessel_ik(void)
{
    double x, nu;

    for (nu = 0.0; nu <= 4.0; nu += 1.0 / 3.0)
        for (x = 0.05; x < 600.0; x *= 1.25) {
            double w = dyn_besseli_scaled(nu, x) * dyn_besselk_scaled(nu + 1.0, x) +
                       dyn_besseli_scaled(nu + 1.0, x) * dyn_besselk_scaled(nu, x);
            const char *band = x < 0.5 ? "bessel Wronskian x<0.5 (series)"
                             : x < 20.0 ? "bessel Wronskian 0.5<=x<20"
                                        : "bessel Wronskian x>=20 (asymptotic)";
            note(band, w, 1.0 / x, x);
        }

    for (nu = 1.0; nu <= 6.0; nu += 1.0)
        for (x = 0.2; x < 100.0; x *= 1.4) {
            note("besseli recurrence",
                 dyn_besseli_scaled(nu - 1.0, x) - dyn_besseli_scaled(nu + 1.0, x),
                 (2.0 * nu / x) * dyn_besseli_scaled(nu, x), x);
            note("besselk recurrence",
                 dyn_besselk_scaled(nu + 1.0, x) - dyn_besselk_scaled(nu - 1.0, x),
                 (2.0 * nu / x) * dyn_besselk_scaled(nu, x), x);
        }

    note_seam("besseli seam at x=20 (0th order)", f_i0, NULL, 20.0, 1.0e-5);
    note_seam("besseli seam at x=20 (1/3 order)", f_i13, NULL, 20.0, 1.0e-5);
    note_seam("besselk seam at x=0.5 (0th order)", f_k0, NULL, 0.5, 1.0e-5);
    note_seam("besselk seam at x=0.5 (1/3 order)", f_k13, NULL, 0.5, 1.0e-5);
    note_seam("besselk seam at x=0.5 (4/3 order)", f_k43, NULL, 0.5, 1.0e-5);

    for (x = 0.2; x < 0.8; x += 0.001) {
        double w = dyn_besseli_scaled(1.0 / 3.0, x) * dyn_besselk_scaled(4.0 / 3.0, x) +
                   dyn_besseli_scaled(4.0 / 3.0, x) * dyn_besselk_scaled(1.0 / 3.0, x);
        note("bessel Wronskian swept through the K seam", w, 1.0 / x, x);
    }
    for (x = 19.0; x < 21.0; x += 0.002) {
        double w = dyn_besseli_scaled(0.0, x) * dyn_besselk_scaled(1.0, x) +
                   dyn_besseli_scaled(1.0, x) * dyn_besselk_scaled(0.0, x);
        note("bessel Wronskian swept through the I seam", w, 1.0 / x, x);
    }

    for (x = 0.05; x < 6.0; x *= 1.4) {
        double im = dyn_besseli_nu(-1.0 / 3.0, x);
        double ip = dyn_besseli_nu(1.0 / 3.0, x);
        double kk = dyn_besselk_nu(1.0 / 3.0, x);
        note_scaled("besseli I_-nu - I_nu = (2/pi) sin(nu pi) K_nu", im - ip,
                    (2.0 / M_PI) * sin(M_PI / 3.0) * kk, fabs(im) + fabs(ip), x);
    }
}

static void t_bessel_jy(void)
{
    double x;
    int n;
    for (n = 1; n <= 5; n++)
        for (x = 0.5; x < 60.0; x *= 1.3) {
            note("besselj recurrence",
                 dyn_besselj(n - 1, x) + dyn_besselj(n + 1, x),
                 (2.0 * n / x) * dyn_besselj(n, x), x);
            note("bessely recurrence",
                 dyn_bessely(n - 1, x) + dyn_bessely(n + 1, x),
                 (2.0 * n / x) * dyn_bessely(n, x), x);
        }
    for (n = 1; n <= 5; n++)
        for (x = 0.5; x < 30.0; x *= 1.6)
            note("besselj negative order", dyn_besselj(-n, x),
                 (n % 2 ? -1.0 : 1.0) * dyn_besselj(n, x), x);
    {
        double re, im;
        dyn_besselh(1, 2, 3.5, &re, &im);
        note("besselh re = J", re, dyn_besselj(2, 3.5), 3.5);
        note("besselh im = Y", im, dyn_bessely(2, 3.5), 3.5);
        dyn_besselh(2, 2, 3.5, &re, &im);
        note("besselh kind 2 im = -Y", im, -dyn_bessely(2, 3.5), 3.5);
    }
}

static void t_elliptic(void)
{
    double m, u, sn, cn, dn;

    note("ellipk(0) = pi/2", dyn_ellipk(0.0), M_PI / 2.0, 0.0);
    note("ellipe(0) = pi/2", dyn_ellipe(0.0), M_PI / 2.0, 0.0);
    note("ellipe(1) = 1", dyn_ellipe(1.0), 1.0, 1.0);

    for (m = 0.01; m < 0.995; m += 0.01) {
        double km = dyn_ellipk(m), em = dyn_ellipe(m);
        double kc = dyn_ellipk(1.0 - m), ec = dyn_ellipe(1.0 - m);
        note("Legendre relation E K' + E' K - K K'", em * kc + ec * km - km * kc,
             M_PI / 2.0, m);
    }

    for (m = 0.0; m <= 1.0; m += 0.05)
        for (u = -6.0; u < 6.0; u += 0.21) {
            if (dyn_ellipj(u, m, &sn, &cn, &dn) != 0)
                continue;
            note("ellipj sn^2+cn^2=1", sn * sn + cn * cn, 1.0, u);
            note("ellipj dn^2+m sn^2=1", dn * dn + m * sn * sn, 1.0, u);
        }

    for (u = -6.0; u < 6.0; u += 0.13) {
        dyn_ellipj(u, 0.0, &sn, &cn, &dn);
        note("ellipj m=0 -> sin", sn, sin(u), u);
        note("ellipj m=0 -> cos", cn, cos(u), u);
        dyn_ellipj(u, 1.0, &sn, &cn, &dn);
        note("ellipj m=1 -> tanh", sn, tanh(u), u);
        note("ellipj m=1 -> sech", cn, 1.0 / cosh(u), u);
    }

    for (u = 0.1; u < 3.0; u += 0.1) {
        double mm = 1.0e-6;
        dyn_ellipj(u, mm, &sn, &cn, &dn);
        note("ellipj small-m expansion [O(m^2) check floor]", sn,
             sin(u) - 0.25 * mm * (u - sin(u) * cos(u)) * cos(u), u);
    }
}

static void t_legendre(void)
{
    double x;
    int n, m;

    for (x = -0.99; x < 0.99; x += 0.017) {
        note("legendre P_0", dyn_legendre(0, 0, x), 1.0, x);
        note("legendre P_1", dyn_legendre(1, 0, x), x, x);
        note("legendre P_2", dyn_legendre(2, 0, x), 0.5 * (3.0 * x * x - 1.0), x);
        note("legendre P_3", dyn_legendre(3, 0, x),
             0.5 * (5.0 * x * x * x - 3.0 * x), x);
        note("legendre P_1^1", dyn_legendre(1, 1, x),
             -sqrt(1.0 - x * x), x);
        note("legendre P_2^1", dyn_legendre(2, 1, x),
             -3.0 * x * sqrt(1.0 - x * x), x);
        note("legendre P_2^2", dyn_legendre(2, 2, x), 3.0 * (1.0 - x * x), x);

        for (m = 0; m <= 4; m++)
            for (n = m + 1; n <= 12; n++)
                note("legendre degree recurrence",
                     (n - m + 1.0) * dyn_legendre(n + 1, m, x),
                     (2.0 * n + 1.0) * x * dyn_legendre(n, m, x) -
                         (n + m) * dyn_legendre(n - 1, m, x),
                     x);
    }
}

static void t_polygamma(void)
{
    double x;
    int n;

    note("digamma(1) = -euler", dyn_digamma(1.0), -0.577215664901532860606512,
         1.0);
    note("polygamma(1,1) = pi^2/6", dyn_polygamma(1, 1.0), M_PI * M_PI / 6.0,
         1.0);

    note("polygamma(1,1/2) = pi^2/2", dyn_polygamma(1, 0.5), M_PI * M_PI / 2.0,
         0.5);

    for (n = 0; n <= 5; n++) {
        double nf = 1.0, i;
        for (i = 1.0; i <= n; i += 1.0)
            nf *= i;
        for (x = 0.2; x < 40.0; x *= 1.3) {
            double want = ((n % 2) ? 1.0 : -1.0) * nf / pow(x, n + 1.0);
            note("polygamma recurrence",
                 dyn_polygamma(n, x) - dyn_polygamma(n, x + 1.0), want, x);
        }
    }

    for (x = 0.3; x < 20.0; x *= 1.2)
        note("digamma duplication", dyn_digamma(2.0 * x),
             0.5 * (dyn_digamma(x) + dyn_digamma(x + 0.5)) + log(2.0), x);
}

static double f_ai(double x, void *u)
{ double v; (void)u; dyn_airy(x, &v, NULL, NULL, NULL); return v; }
static double f_aip(double x, void *u)
{ double v; (void)u; dyn_airy(x, NULL, &v, NULL, NULL); return v; }
static double f_bi(double x, void *u)
{ double v; (void)u; dyn_airy(x, NULL, NULL, &v, NULL); return v; }
static double f_bip(double x, void *u)
{ double v; (void)u; dyn_airy(x, NULL, NULL, NULL, &v); return v; }

static void t_airy(void)
{
    double x, ai, aip, bi, bip;

    dyn_airy(0.0, &ai, &aip, &bi, &bip);
    note("Ai(0)",  ai,  pow(3.0, -2.0 / 3.0) / tgamma(2.0 / 3.0), 0.0);
    note("Ai'(0)", aip, -pow(3.0, -1.0 / 3.0) / tgamma(1.0 / 3.0), 0.0);
    note("Bi(0)",  bi,  pow(3.0, -1.0 / 6.0) / tgamma(2.0 / 3.0), 0.0);
    note("Bi'(0)", bip, pow(3.0, 1.0 / 6.0) / tgamma(1.0 / 3.0), 0.0);

    for (x = -30.0; x <= 30.0; x += 0.01) {
        const char *band;
        dyn_airy(x, &ai, &aip, &bi, &bip);
        band = (x >= 0.1)   ? "airy Wronskian x>=0.1 (Bessel)"
             : (x >= -7.0)  ? "airy Wronskian -7<=x<0.1 (series)"
             : (x >= -12.0) ? "airy Wronskian -12<=x<-7 (asymptotic, the band)"
                            : "airy Wronskian x<-12 (asymptotic)";
        note(band, ai * bip - aip * bi, 1.0 / M_PI, x);
    }

    note_seam("airy seam at x=0.1 (Ai)",  f_ai,  NULL, 0.1,   1.0e-5);
    note_seam("airy seam at x=0.1 (Bi)",  f_bi,  NULL, 0.1,   1.0e-5);
    note_seam("airy seam at x=0.1 (Ai')", f_aip, NULL, 0.1,   1.0e-5);
    note_seam("airy seam at x=0.1 (Bi')", f_bip, NULL, 0.1,   1.0e-5);
    note_seam("airy seam at x=-7 (Ai) [the band]",  f_ai,  NULL, -7.0, 1.0e-5);
    note_seam("airy seam at x=-7 (Bi) [the band]",  f_bi,  NULL, -7.0, 1.0e-5);
    note_seam("airy seam at x=-7 (Ai') [the band]", f_aip, NULL, -7.0, 1.0e-5);
    note_seam("airy seam at x=-7 (Bi') [the band]", f_bip, NULL, -7.0, 1.0e-5);

    for (x = -6.0; x < 6.0; x += 0.5) {
        double h = 1.0e-3, y[5], k;
        int i;
        for (i = 0; i < 5; i++) {
            double xx = x + (i - 2) * h;
            dyn_airy(xx, &y[i], NULL, NULL, NULL);
        }
        k = (-y[0] + 16.0 * y[1] - 30.0 * y[2] + 16.0 * y[3] - y[4]) /
            (12.0 * h * h);
        if (fabs(x * y[2]) > 1.0e-6)
            note("airy ODE y''=xy (finite difference, ~1e-9 floor)", k,
                 x * y[2], x);
    }
}

int main(void)
{
    int i, bad = 0;

    t_gammainc();
    t_gammaincinv();
    t_betainc();
    t_betaincinv();
    t_bessel_ik();
    t_bessel_jy();
    t_elliptic();
    t_legendre();
    t_polygamma();
    t_airy();

    printf("\n%-52s %12s %12s %10s\n", "regime", "worst rel", "at x", "checks");
    printf("%-52s %12s %12s %10s\n", "----------------------------------------",
           "---------", "-----", "------");
    for (i = 0; i < n_regimes; i++) {
        printf("%-52s %12.3e %12.4g %10lld\n", R[i].name, R[i].worst, R[i].at,
               R[i].n);
        {
            double bar = 1.0e-12;
            if (strstr(R[i].name, "finite difference"))
                bar = 1.0e-6;
            else if (strstr(R[i].name, "[O(m^2) check floor]"))
                bar = 5.0e-12;
            else if (strstr(R[i].name, "the band"))
                bar = 1.0e-9;
            else if (strstr(R[i].name, "round trip"))
                bar = 1.0e-11;
            else if (strstr(R[i].name, "series"))
                bar = 1.0e-11;
            if (!(R[i].worst <= bar)) {
                printf("    ^^ OVER BAR %g\n", bar);
                bad++;
            }
        }
    }
    printf("\n%d regime(s) over bar\n", bad);
    return bad ? 1 : 0;
}
