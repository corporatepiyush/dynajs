// test-mathx-debye: besseli/besselk at order nu >= 40 (Debye uniform asymptotics,
// DLMF 10.41) held to 4 ULP against oracles that share no code or method with it.
//   K_{n+1/2}(x) e^x = sqrt(pi/(2x)) sum_{k=0..n} (n+k)! / (k! (n-k)! (2x)^k)
//     -- a finite sum of positive terms (DLMF 10.49.12), so double-double
//     accumulation with exponent tracking is accurate to ~1e-28.
//   I_n(x) = sum_k (x/2)^(2k+n) / (k! (n+k)!) -- positive terms again.
//   e^-x for the unscaled/scaled conversions: double-double Taylor after
//   k ln2 reduction and 2^-10 halving.
// The oracle error is ~1e-28 relative, so every reported ULP (fractional,
// measured against the double-double oracle) is the implementation's.
// Regimes: orders 40..48 dense (where a dropped U_k term first shows),
// orders 40..1500 for x = 1e-3 nu .. 30 nu, huge x (1e6..1e300) for the scaled
// K (the nu^2 / (s + x) form), orders 1e3..1e6 at x near the turning point
// where the unscaled values are representable (bisection on nu eta = Y,
// |Y| <= 600), and scaled K at x ~ nu^2 / (2Y). Means are printed, not gated.
// Below-bar (equivalent at 4 ULP) mutants, measured: 9 instead of 10 terms,
// log tail cut at j = 12/13 instead of 14, the low words of 2 pi and pi/2.
// Generic (non-integer) orders: 149 mpmath 1.3.0 rows, nu 40..6826, K from the
// reflection formula pi/2 (I_-nu - I_nu) / sin(nu pi) at 50+ digits, worst 1 ULP
// (fixC wave-5 report; the pinned mathx vectors also pass at 4 ULP).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dyn-mathx.h"

static int int_in(double v, int lo, int hi)
{
    if (!(v >= (double)lo && v < (double)hi + 1.0)) {
        fprintf(stderr, "int_in: %g outside [%d, %d]\n", v, lo, hi);
        abort();
    }
    return (int)v;
}

typedef struct {
    double hi, lo;
} dd;

static dd mk(double hi, double lo)
{
    dd r;
    double s = hi + lo;
    r.lo = lo - (s - hi);
    r.hi = s;
    return r;
}

static dd add(dd a, dd b)
{
    double s = a.hi + b.hi, bb = s - a.hi, e = (a.hi - (s - bb)) + (b.hi - bb);
    return mk(s, e + a.lo + b.lo);
}

static dd mul(dd a, dd b)
{
    double p = a.hi * b.hi, e = fma(a.hi, b.hi, -p);
    return mk(p, e + a.hi * b.lo + a.lo * b.hi);
}

static dd muld(dd a, double b)
{
    double p = a.hi * b, e = fma(a.hi, b, -p);
    return mk(p, e + a.lo * b);
}

static dd divd(dd a, double b)
{
    double q = a.hi / b, p = q * b, e = fma(q, b, -p);
    return mk(q, ((a.hi - p) - e + a.lo) / b);
}

static dd ddiv(dd a, dd b)
{
    double q = a.hi / b.hi;
    dd r = add(a, muld(b, -q));
    return add(mk(q, 0.0), mk(r.hi / b.hi, 0.0));
}

static dd scale2(dd a, int e)
{
    return mk(ldexp(a.hi, e), ldexp(a.lo, e));
}

static dd dsqrt(dd a)
{
    double r = sqrt(a.hi);
    return mk(r, (fma(-r, r, a.hi) + a.lo) / (2.0 * r));
}

static const dd LN2 = { .hi = 0.6931471805599453, .lo = 2.3190468138462996e-17 };
static const dd HALF_PI = { .hi = 1.5707963267948966, .lo = 6.123233995736766e-17 };

static dd dexp_neg(double x, int64_t* e2)
{
    double k = floor(x / LN2.hi + 0.5);
    dd r = add(mk(-x, 0.0), muld(LN2, k)), s = mk(1.0, 0.0), t = mk(1.0, 0.0);
    int j;
    r = scale2(r, -10);
    for (j = 1; j < 20; j++) {
        t = divd(mul(t, r), (double)j);
        s = add(s, t);
    }
    for (j = 0; j < 10; j++)
        s = mul(s, s);
    *e2 = -(int64_t)k;
    return s;
}

static dd norm(dd a, int64_t* e)
{
    int k;
    (void)frexp(a.hi, &k);
    *e += k;
    return scale2(a, -k);
}

static double ulps(double got, dd m, int64_t e)
{
    double want, u;
    int k;
    if (e > 1100)
        return isinf(got) ? 0.0 : 1e300;
    if (e < -1200)
        return got == 0.0 ? 0.0 : 1e300;
    want = ldexp(m.hi, (int)e);
    if (want == 0.0)
        return got == 0.0 ? 0.0 : fabs(got) / 4.9406564584124654e-324;
    if (isinf(want))
        return isinf(got) ? 0.0 : 1e300;
    (void)frexp(want, &k);
    u = ldexp(1.0, k - 53 < -1074 ? -1074 : k - 53);
    if (!isfinite(got))
        return 1e300;
    return fabs((ldexp(got, (int)-e) - m.hi) - m.lo) / ldexp(u, (int)-e);
}

static dd k_half_scaled(int n, double x, int64_t* e)
{
    dd s = mk(1.0, 0.0), t = mk(1.0, 0.0);
    int64_t se = 0;
    int k;
    for (k = 0; k < n; k++) {
        t = divd(divd(muld(t, (double)(n + k + 1) * (double)(n - k)), (double)(k + 1)), 2.0 * x);
        s = add(s, t);
        if (s.hi > 0x1p600 || s.hi < 0x1p-600) {
            int64_t z = 0;
            s = norm(s, &z);
            t = scale2(t, -(int)z);
            se += z;
        }
    }
    *e = se;
    s = mul(s, dsqrt(divd(HALF_PI, x)));
    return norm(s, e);
}

static dd i_int(int n, double x, int64_t* e)
{
    dd p = mk(1.0, 0.0), q, s, t;
    int64_t pe = 0;
    double h = 0.5 * x;
    int j;
    for (j = 1; j <= n; j++) {
        p = divd(muld(p, h), (double)j);
        p = norm(p, &pe);
    }
    q = mul(mk(h, 0.0), mk(h, 0.0));
    s = mk(1.0, 0.0);
    t = mk(1.0, 0.0);
    for (j = 1; j < 100000; j++) {
        t = divd(mul(t, q), (double)j * (double)(n + j));
        s = add(s, t);
        if (t.hi < s.hi * 1e-34)
            break;
        if (s.hi > 0x1p600) {
            s = scale2(s, -600);
            t = scale2(t, -600);
            pe += 600;
        }
    }
    *e = pe;
    return norm(mul(p, s), e);
}

typedef struct {
    const char* name;
    double worst, nu, x, sum;
    long n;
} stat;

static stat S[11];
static int big;
static int fails;

static void rec(int i, const char* name, double e, double nu, double x)
{
    static const char* const bn[4] = { "besselkScaled nu 1e3..1e6", "besselk nu 1e3..1e6", "besseli nu 1e3..1e6", "besseliScaled nu 1e3..1e6" };
    if (big && i < 4) {
        name = bn[i];
        i += 5;
    }
    S[i].name = name;
    S[i].n++;
    S[i].sum += e;
    if (!(e <= S[i].worst)) {
        S[i].worst = e;
        S[i].nu = nu;
        S[i].x = x;
    }
}

static uint64_t rng = 0x9e3779b97f4a7c15u;

static double unif(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) * 0x1p-53;
}

static void check_k(int n, double x)
{
    double nu = n + 0.5;
    int64_t e, ee;
    dd ks = k_half_scaled(n, x, &e), em = dexp_neg(x, &ee), ku;
    int64_t ue = e + ee;
    rec(0, "besselkScaled half-integer", ulps(dyn_besselk_scaled(nu, x), ks, e), nu, x);
    ku = norm(mul(ks, em), &ue);
    rec(1, "besselk half-integer", ulps(dyn_besselk_nu(nu, x), ku, ue), nu, x);
}

static void check_i(int n, double x)
{
    int64_t e, ee;
    dd iv = i_int(n, x, &e), em = dexp_neg(x, &ee), is;
    int64_t se = e + ee;
    rec(2, "besseli integer", ulps(dyn_besseli_nu((double)n, x), iv, e), (double)n, x);
    is = norm(mul(iv, em), &se);
    rec(3, "besseliScaled integer", ulps(dyn_besseli_scaled((double)n, x), is, se), (double)n, x);
}

static double turning(double nu, double y)
{
    double lo = 1e-3, hi = 10.0, z = 1.0;
    int j;
    for (j = 0; j < 200; j++) {
        double r;
        z = 0.5 * (lo + hi);
        r = sqrt(1.0 + z * z);
        if (nu * (r + log(z / (1.0 + r))) < y)
            lo = z;
        else
            hi = z;
    }
    return z;
}

int main(void)
{
    int i, n;
    static const double fr[] = { 1e-3, 0.01, 0.1, 0.3, 0.7, 0.95, 1.0, 1.05, 1.5, 3.0, 10.0, 30.0 };
    for (n = 40; n <= 48; n++)
        for (i = 0; i < (int)(sizeof(fr) / sizeof(fr[0])); i++) {
            check_k(n, (n + 0.5) * fr[i]);
            check_i(n, n * fr[i]);
        }
    for (i = 0; i < 6000; i++) {
        int nk = 40 + (int)(unif() * 9.0), ni;
        double x = nk * exp(log(1e-3) + unif() * log(3e4));
        check_k(nk, x);
        ni = 40 + (int)(unif() * 9.0);
        x = ni * exp(log(1e-3) + unif() * log(3e4));
        check_i(ni, x < 4000.0 ? x : 3999.0);
    }
    for (i = 0; i < 1500; i++) {
        int nk = 40 + int_in(exp(unif() * log(1460.0)), 0, 1460);
        double f = exp(log(1e-3) + unif() * log(3e4));
        check_k(nk, (nk + 0.5) * f);
        if (nk * f < 4000.0)
            check_i(nk, nk * f);
    }
    big = 1;
    for (i = 0; i < 160; i++) {
        int nk = int_in(exp(log(1000.0) + unif() * log(1000.0)), 1000, 1000000);
        double y = -600.0 + 1200.0 * unif();
        double zk = turning(nk + 0.5, y), zi = turning((double)nk, y);
        check_k(nk, (nk + 0.5) * zk);
        check_i(nk, nk * zi);
    }
    big = 0;
    for (i = 0; i < 400; i++) {
        int nk = 40 + (int)(unif() * 30.0);
        double x = exp(log(1e6) + unif() * (log(1e300) - log(1e6)));
        int64_t e;
        dd ks = k_half_scaled(nk, x, &e);
        rec(4, "besselkScaled huge x", ulps(dyn_besselk_scaled(nk + 0.5, x), ks, e), nk + 0.5, x);
    }
    for (i = 0; i < 120; i++) {
        int nk = int_in(exp(log(1000.0) + unif() * log(1000.0)), 1000, 1000000);
        double y = 1.0 + 599.0 * unif(), x = (nk + 0.5) * (nk + 0.5) / (2.0 * y);
        int64_t e;
        dd ks = k_half_scaled(nk, x, &e);
        rec(9, "besselkScaled nu 1e3..1e6, x ~ nu^2", ulps(dyn_besselk_scaled(nk + 0.5, x), ks, e), nk + 0.5, x);
    }
    for (i = 0; i < 10; i++) {
        printf("%-28s n=%-6ld worst %.2f ULP (mean %.3f) at nu=%.17g x=%.17g\n", S[i].name, S[i].n, S[i].worst, S[i].sum / (double)S[i].n, S[i].nu, S[i].x);
        if (S[i].worst > 4.0)
            fails++;
    }
    printf("test_mathx_debye: %s\n", fails ? "FAIL (bar 4 ULP)" : "all within 4 ULP");
    return fails ? 1 : 0;
}
