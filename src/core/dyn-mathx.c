/*
 * dyn-mathx -- MATLAB tier B special functions, pure C. See dyn-mathx.h for
 * the per-regime accuracy contract; this file is where the regimes live.
 *
 * Every routine here is a classical algorithm, and the reason each was picked
 * over the obvious alternative is stated at the routine. The recurring theme is
 * that the obvious alternative is usually a single expansion applied over the
 * whole domain, and that is exactly what produces an accuracy cliff: a series
 * that converges beautifully at one end of the domain is cancelling six digits
 * at the other. So each function is a pair of methods with an OVERLAPPING
 * switch point, chosen so neither is used where it is weak.
 */
#include "dyn-mathx.h"

#include <float.h>
#include <limits.h>     /* INT_MAX/INT_MIN: the Bessel order is an int */
/* Darwin's <math.h> only declares the reentrant lgamma_r when _REENTRANT is
 * defined before the header is processed; glibc/musl expose it unconditionally.
 * Defining it here, ahead of this TU's first #include <math.h>, is a no-op on
 * the platforms that do not gate it. The binding this was extracted from makes
 * the same move, for the same reason. */
#define _REENTRANT
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MX_EPS      3.0e-16
#define MX_FPMIN    1.0e-300
#define MX_ITMAX    400

static double mx_lgamma(double x)
{
    int sign;
    /* lgamma_r rather than lgamma: the non-reentrant spelling reports its sign
     * through the global `signgam`, which makes it a data race the moment two
     * threads call it. The binding already made this choice; the core keeps
     * it. */
#if defined(_WIN32)
    sign = 0;
    (void)sign;
    return lgamma(x);
#else
    return lgamma_r(x, &sign);
#endif
}

/* ====================================================================
 * Incomplete gamma
 * ==================================================================== */

/* Ascending series for P(a,x). Converges fast for x < a+1; for larger x it
 * still converges but needs many terms and starts cancelling. */
static double mx_gser(double a, double x)
{
    double sum, del, ap;
    int n;

    if (x <= 0.0)
        return 0.0;

    ap = a;
    del = sum = 1.0 / a;
    for (n = 0; n < MX_ITMAX; n++) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (fabs(del) < fabs(sum) * MX_EPS)
            break;
    }
    return sum * exp(-x + a * log(x) - mx_lgamma(a));
}

/* Modified Lentz continued fraction for Q(a,x). The complementary half of the
 * pair: fast where the series is slow. */
static double mx_gcf(double a, double x)
{
    double b, c, d, h, an, del;
    int i;

    b = x + 1.0 - a;
    c = 1.0 / MX_FPMIN;
    d = 1.0 / b;
    h = d;
    for (i = 1; i <= MX_ITMAX; i++) {
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
    return exp(-x + a * log(x) - mx_lgamma(a)) * h;
}

double dyn_gammainc_p(double a, double x)
{
    if (!(x >= 0.0) || !(a > 0.0))
        return NAN;
    if (x == 0.0)
        return 0.0;
    if (x < a + 1.0)
        return mx_gser(a, x);
    return 1.0 - mx_gcf(a, x);
}

double dyn_gammainc_q(double a, double x)
{
    if (!(x >= 0.0) || !(a > 0.0))
        return NAN;
    if (x == 0.0)
        return 1.0;
    /* Computed directly rather than as 1 - P: for x >> a the upper tail is
     * tiny and `1 - P` would deliver it with absolute, not relative,
     * accuracy -- the difference between 1e-17 and 0. */
    if (x < a + 1.0)
        return 1.0 - mx_gser(a, x);
    return mx_gcf(a, x);
}

double dyn_gammaincinv(double a, double p)
{
    double x, err, t, u, a1, lna1, afac, pp, lga;
    int j;

    if (!(a > 0.0) || !(p >= 0.0) || !(p <= 1.0))
        return NAN;
    if (p == 0.0)
        return 0.0;
    if (p == 1.0)
        return INFINITY;

    a1 = a - 1.0;
    lna1 = 0.0;
    afac = 0.0;
    /* lgamma(a) is loop-invariant, and the a <= 1 branch used to recompute it
     * on every iteration of a Newton loop that runs up to 24 times.
     *
     * It is computed HERE, in that branch only, and not once above the if:
     * hoisting it above cost the a > 1 arm 2% (reproducible over three runs)
     * for code it does not execute. Leave the arm that does not need a change
     * byte-for-byte what it was. */
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
        /* Halley rather than plain Newton: the second-derivative correction
         * costs one division and roughly halves the iteration count on the
         * tails, where the plain step overshoots into x <= 0. */
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

/* ====================================================================
 * Incomplete beta
 * ==================================================================== */

static double mx_betacf(double a, double b, double x)
{
    double qab, qap, qam, c, d, h, aa, del;
    int m, m2;

    qab = a + b;
    qap = a + 1.0;
    qam = a - 1.0;
    c = 1.0;
    d = 1.0 - qab * x / qap;
    if (fabs(d) < MX_FPMIN)
        d = MX_FPMIN;
    d = 1.0 / d;
    h = d;
    for (m = 1; m <= MX_ITMAX; m++) {
        m2 = 2 * m;
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
    return h;
}

double dyn_betainc(double x, double a, double b)
{
    double bt;

    if (!(a > 0.0) || !(b > 0.0) || !(x >= 0.0) || !(x <= 1.0))
        return NAN;
    if (x == 0.0)
        return 0.0;
    if (x == 1.0)
        return 1.0;

    bt = exp(mx_lgamma(a + b) - mx_lgamma(a) - mx_lgamma(b) + a * log(x) +
             b * log1p(-x));
    /* The fraction converges rapidly only on one side of the mode; reflect so
     * it is always evaluated there. */
    if (x < (a + 1.0) / (a + b + 2.0))
        return bt * mx_betacf(a, b, x) / a;
    return 1.0 - bt * mx_betacf(b, a, 1.0 - x) / b;
}

double dyn_betaincinv(double p, double a, double b)
{
    double x, t, u, w, err, a1, b1, afac, pp, al, h;
    int j;

    if (!(a > 0.0) || !(b > 0.0) || !(p >= 0.0) || !(p <= 1.0))
        return NAN;
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
        w = (x * sqrt(al + h) / h) -
            (1.0 / (2.0 * b - 1.0) - 1.0 / (2.0 * a - 1.0)) *
                (al + 5.0 / 6.0 - 2.0 / (3.0 * h));
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

/* ====================================================================
 * Modified Bessel I and K
 * ==================================================================== */

/* Ascending series, valid for every x but used only for x <= MX_I_SERIES_MAX:
 * every term is positive so there is no cancellation at all, and the accuracy
 * is simply tgamma's. The cost is the term count, which grows like x. */
#define MX_I_SERIES_MAX 20.0

static double mx_besseli_series(double nu, double x, int scaled)
{
    double half = 0.5 * x, term, sum, k;
    int i;

    /* term_0 = (x/2)^nu / Gamma(nu+1), built in the log domain so a large nu
     * does not overflow on the way to a finite answer. */
    term = exp(nu * log(half) - mx_lgamma(nu + 1.0));
    sum = term;
    for (i = 1; i < 300; i++) {
        k = (double)i;
        term *= (half * half) / (k * (nu + k));
        sum += term;
        if (term < sum * 1.0e-18)
            break;
    }
    return scaled ? sum * exp(-x) : sum;
}

/* Hankel asymptotic expansion for large x. Optimal truncation error is
 * O(e^-2x), which at the x = 20 switch point is 4e-18 -- below the double
 * epsilon, which is why the switch is there and not lower. */
static double mx_besseli_asym(double nu, double x, int scaled)
{
    double mu = 4.0 * nu * nu, sum = 1.0, term = 1.0, prev = HUGE_VAL;
    int k;

    for (k = 1; k < 40; k++) {
        double kk = (double)k;
        double f = (mu - (2.0 * kk - 1.0) * (2.0 * kk - 1.0)) / (kk * 8.0 * x);
        term *= -f;
        if (fabs(term) > prev) /* asymptotic: stop at the smallest term */
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
    /* NEGATIVE ORDER IS NOT A FOLD. I_-nu = I_nu holds only for INTEGER nu;
     * in general I_-nu = I_nu + (2/pi) sin(nu pi) K_nu, and Airy needs exactly
     * the orders where that extra term is largest (-1/3, -2/3). The series
     * below is valid as written for any nu > -1 -- Gamma(nu+k+1) is finite and
     * nu+k > 0 for every k >= 1 -- so the negative fractional orders need no
     * special case at all, only the integer ones below -1 do. */
    if (nu <= -1.0) {
        double f = nu - floor(nu);
        if (f < 1.0e-12 || f > 1.0 - 1.0e-12)
            nu = -nu; /* integer: the fold is exact */
        else
            return NAN; /* not reachable from this core's callers */
    }
    if (!(x > 0.0)) {
        if (x == 0.0)
            return (nu == 0.0) ? 1.0 : (nu > 0.0 ? 0.0 : INFINITY);
        return NAN;
    }
    if (x <= MX_I_SERIES_MAX)
        return mx_besseli_series(nu, x, 1);
    /* The Hankel expansion depends on nu only through mu = 4 nu^2, so it is
     * the same series for +nu and -nu. That is correct rather than sloppy:
     * I_-nu - I_nu = (2/pi) sin(nu pi) K_nu is O(e^-x), so relative to
     * I_nu ~ e^x the two differ by O(e^-2x) = 4e-18 at the x = 20 switch --
     * below the double epsilon, and shrinking. */
    return mx_besseli_asym(nu, x, 1);
}

double dyn_besseli_nu(double nu, double x)
{
    double s = dyn_besseli_scaled(nu, x);
    return s * exp(x);
}

/* K by quadrature of  K_nu(x) e^x = int_0^inf exp(-x(cosh t - 1)) cosh(nu t) dt.
 *
 * The trapezoid rule is not a compromise here, it is the right tool: the
 * integrand is analytic in a strip of half-width pi/2 about the real axis (for
 * |Im t| < pi/2, Re cosh t stays positive, so the exponential still decays) and
 * it decays doubly exponentially, and for exactly that class the trapezoid
 * error is O(e^(-pi^2/h)) rather than O(h^2). At h = 0.06 that exponent is -164.
 *
 * The step has to narrow as x grows, because the peak at t = 0 has width
 * 1/sqrt(x): a fixed step would undersample it and the geometric convergence
 * would silently become garbage. That is the one way this routine can be wrong
 * without looking wrong, so it is the thing the test hammers.
 */
static double mx_besselk_quad(double nu, double x)
{
    double h, sum, t, f, cutoff;
    int k;

    h = 0.25 / sqrt(x);
    if (h > 0.06)
        h = 0.06;
    cutoff = 60.0 + fabs(nu) * 4.0;

    sum = 0.5; /* t = 0: exp(0) * cosh(0) = 1, halved by the trapezoid rule */
    for (k = 1; k < 200000; k++) {
        double arg;
        t = (double)k * h;
        arg = x * (cosh(t) - 1.0) - nu * t;
        if (arg > cutoff)
            break;
        f = exp(-x * (cosh(t) - 1.0)) * cosh(nu * t);
        sum += f;
        if (f < 1.0e-20 && t > 1.0)
            break;
    }
    return sum * h;
}

/* Below x = 0.5 the quadrature would need cosh t past the overflow point, so
 * the series forms take over. For a non-integer order the connection formula
 * K = (pi/2)(I_-nu - I_nu)/sin(nu pi) is exact and well conditioned as long as
 * nu is not near an integer.
 *
 * IT IS ONLY VALID FOR 0 < nu < 1, and that restriction is load-bearing rather
 * than incidental: I_-nu is evaluated by the ascending series, whose leading
 * term carries 1/Gamma(1-nu), and mx_lgamma returns log|Gamma| -- it throws the
 * SIGN away. For nu > 1 the argument 1-nu passes a pole and Gamma goes
 * negative, so the series silently returns a positive number where a negative
 * one belongs. That produced a Wronskian of the wrong sign entirely (relative
 * error 2.0), which is what the identity check caught and what no round trip
 * would have. Higher orders go through the recurrence below instead. */
static double mx_k_connection(double nu, double x)
{
    double im = mx_besseli_series(-nu, x, 0);
    double ip = mx_besseli_series(nu, x, 0);
    return M_PI * 0.5 * (im - ip) / sin(nu * M_PI);
}

/* Integer order, small x: Abramowitz & Stegun 9.6.11. The log term and the
 * psi-weighted series both grow with x, and they cancel -- which is precisely
 * why this is confined to x < 0.5, where the cancellation costs under one
 * digit. At x = 9 it would cost eight. */
static double mx_besselk_small_int(int n, double x)
{
    double half = 0.5 * x, hh = half * half;
    double sum, psi1, psi2, logh, res;
    int k, j;
    const double EULER = 0.57721566490153286061;

    logh = log(half);

    if (n == 0) {
        /* K_0 = -log(x/2) I_0 + sum_k psi(k+1) (x^2/4)^k / (k!)^2 */
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

    /* K_n, n >= 1: the finite descending sum, the log term, and the ascending
     * psi-weighted series. */
    res = 0.0;
    {
        double pw = pow(half, -(double)n) * 0.5;
        double fact_nk1 = 1.0; /* (n-k-1)! */
        double kfact = 1.0;    /* k!       */
        double sgn_pow = 1.0;  /* (-x^2/4)^k */
        for (j = 1; j < n; j++)
            fact_nk1 *= (double)j; /* (n-1)! */
        for (k = 0; k < n; k++) {
            if (k > 0) {
                kfact *= (double)k;
                fact_nk1 /= (double)(n - k); /* (n-k-1)! from (n-k)! */
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
            nkfact *= (double)j; /* n! */
        psi1 = -EULER;           /* psi(1)   */
        psi2 = -EULER;           /* psi(n+1) */
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

/* Small x, any order >= 0. Anchors on the two lowest orders -- which are the
 * only ones the series forms are conditioned for -- and walks up with
 *     K_(v+1) = K_(v-1) + (2v/x) K_v
 * which is the STABLE direction for K (it is the growing solution). The
 * corresponding upward recurrence for I is unstable and is not used anywhere
 * in this file for that reason. */
static double mx_besselk_small(double nu, double x)
{
    double nu0, ka, kb, kc;
    double frac = nu - floor(nu);
    int steps, j;

    if (frac < 1.0e-12 || frac > 1.0 - 1.0e-12) {
        nu0 = 0.0;
        steps = (int)floor(nu + 0.5);
        ka = mx_besselk_small_int(0, x);
        kb = mx_besselk_small_int(1, x);
    } else {
        nu0 = frac;
        steps = (int)floor(nu);
        ka = mx_k_connection(nu0, x);
        /* K_(nu0+1) from the recurrence run once, using K_(nu0-1) = K_(1-nu0)
         * -- an order back in (0,1), where the connection formula is valid. */
        kb = mx_k_connection(1.0 - nu0, x) + (2.0 * nu0 / x) * ka;
    }

    if (steps <= 0)
        return ka;
    for (j = 1; j <= steps - 1; j++) {
        kc = ka + (2.0 * (nu0 + (double)j) / x) * kb;
        ka = kb;
        kb = kc;
    }
    return kb;
}

double dyn_besselk_scaled(double nu, double x)
{
    /* NaN/Inf orders are not representable in the branches below (the
       (int)floor steps in the small-x series are UB on NaN). The limits
       are K_nu(x) -> 0 as |nu| -> inf, and K_nu(x) ~ sqrt(pi/2x) e^-x -> 0
       as x -> +inf; scipy's kv agrees on all three. */
    if (isnan(nu) || isnan(x))
        return NAN;
    if (!(x > 0.0))
        return (x == 0.0) ? INFINITY : NAN;
    /* Huge FINITE order is the infinity limit for every x > 0, and letting it
     * reach the recurrence's (int)floor(nu) cast would be out-of-range UB
     * (besselj/bessely refuse at INT_MAX; K takes fractional orders, so the
     * same hazard arrives as a double). Guard sits AFTER the x==0 branch:
     * K_nu(0) is +inf for every finite nu, huge or not. */
    if (fabs(nu) > 9.0e15 || isinf(nu) || x == INFINITY)
        return 0.0;
    if (nu < 0.0)
        nu = -nu; /* K_-nu = K_nu, exactly, for every order */

    if (x >= 0.5)
        return mx_besselk_quad(nu, x);
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
        /* I_n(-x) = (-1)^n I_n(x) */
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

/* ---- ordinary Bessel: straight to libm ---------------------------------- */

/* -INT_MIN is not representable and stays INT_MIN, so negating the order and
   recursing never terminated: dyn_besselj(INT_MIN, x) was an unbounded
   self-call. Fold the sign in unsigned arithmetic instead of recursing. */
static unsigned int bessel_order_abs(int n, int *neg)
{
    unsigned int m = (n < 0) ? (unsigned int)(-(long long)n) : (unsigned int)n;
    *neg = (n < 0) && (m & 1u);         /* J_-n = (-1)^n J_n, same for Y */
    if (m > (unsigned int)INT_MAX)
        m = (unsigned int)INT_MAX;
    return m;
}

/* libm's jn runs an O(n) RECURRENCE: jn(INT_MAX, 3) measured 11646 ms for an
   answer of 0. |J_n(x)| <= (|x|/2)^n / n!, so once that bound is below the
   smallest subnormal the recurrence cannot produce anything but zero. */
static int bessel_underflows(unsigned int m, double x)
{
    double ax = fabs(x);
    if (m < 2u)
        return 0;
    if (ax == 0.0)
        return 1;                      /* J_n(0) = 0 for every n >= 1 */
    return (double)m * log(ax * 0.5) - lgamma((double)m + 1.0) < -745.0;
}

/* The underflow bound cannot fire when |x| is comparable to n, and there the
   recurrence runs to completion: measured 2668 ms at n = x = INT_MAX, linear in
   n. Refuse past a bounded order rather than block the caller for seconds. */
#define DYN_BESSEL_MAX_ORDER (1u << 24)   /* ~20 ms of recurrence */

/* Bounded means EITHER the underflow bound answers it outright OR the order is
   small enough to recurse. besselj(INT_MAX, 3) is bounded; besselj(INT_MAX,
   INT_MAX) is not. bessely has no underflow path, so it asks with x = 0. */
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
        return NAN;
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
        return NAN;
    if (m == 1)
        v = y1(x);
    else
        v = yn((int)m, x);
    return neg ? -v : v;
}

int dyn_besselh(int kind, int n, double x, double *re, double *im)
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

/* ====================================================================
 * Elliptic integrals
 * ==================================================================== */

double dyn_ellipk(double m)
{
    double a, b, t;
    int i;

    if (!(m <= 1.0))
        return NAN;
    if (m == 1.0)
        return INFINITY;

    a = 1.0;
    b = sqrt(1.0 - m);
    /* AGM converges quadratically: 5 iterations already exceed double
     * precision, and 30 is a bound not an expectation. */
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
        return NAN;
    if (m == 1.0)
        return 1.0;

    a = 1.0;
    b = sqrt(1.0 - m);
    c = sqrt(m);
    sum = 0.5 * c * c; /* the n = 0 term, 2^(n-1) c_n^2 with n = 0 */
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

int dyn_ellipj(double u, double m, double *sn, double *cn, double *dn)
{
    double a[32], c[32], t, phi;
    int i, n;

    if (!(m >= 0.0) || !(m <= 1.0))
        return -1;

    if (m < 1.0e-14) { /* degenerate: circular functions */
        double s = sin(u), co = cos(u);
        if (sn) *sn = s;
        if (cn) *cn = co;
        if (dn) *dn = 1.0 - 0.5 * m * s * s;
        return 0;
    }
    if (1.0 - m < 1.0e-14) { /* degenerate: hyperbolic functions */
        double th = tanh(u), ch = cosh(u);
        if (sn) *sn = th;
        if (cn) *cn = 1.0 / ch;
        if (dn) *dn = 1.0 / ch;
        return 0;
    }

    /* Descending Landen via the AGM: build the ladder, evaluate at the top
     * where the modulus is zero and the functions are circular, then walk back
     * down. This is the standard construction and it is unconditionally
     * stable, unlike the ascending direction. */
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
        if (s > 1.0)  s = 1.0;
        if (s < -1.0) s = -1.0;
        phi = 0.5 * (asin(s) + phi);
    }

    {
        double s = sin(phi), co = cos(phi);
        if (sn) *sn = s;
        if (cn) *cn = co;
        if (dn) *dn = sqrt(1.0 - m * s * s);
    }
    return 0;
}

/* ====================================================================
 * Associated Legendre
 * ==================================================================== */

double dyn_legendre(int n, int m, double x)
{
    double pmm, pmmp1, pll, somx2, fact;
    int i, ll;

    if (m < 0 || n < 0 || m > n || fabs(x) > 1.0)
        return NAN;

    pmm = 1.0;
    if (m > 0) {
        somx2 = sqrt((1.0 - x) * (1.0 + x));
        fact = 1.0;
        for (i = 1; i <= m; i++) {
            pmm *= -fact * somx2; /* the minus is the Condon-Shortley phase */
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
        pll = (x * (2.0 * ll - 1.0) * pmmp1 - (ll + m - 1.0) * pmm) /
              (double)(ll - m);
        pmm = pmmp1;
        pmmp1 = pll;
    }
    return pll;
}

/* ====================================================================
 * Digamma and polygamma
 * ==================================================================== */

/* Bernoulli numbers B_2k, k = 1.. -- the Euler-Maclaurin tail. */
static const double MX_B2K[] = {
    1.0 / 6.0,          -1.0 / 30.0,        1.0 / 42.0,
    -1.0 / 30.0,        5.0 / 66.0,         -691.0 / 2730.0,
    7.0 / 6.0,          -3617.0 / 510.0,    43867.0 / 798.0,
    -174611.0 / 330.0
};

double dyn_digamma(double x)
{
    double r = 0.0, f, x2;

    if (isnan(x))
        return NAN;
    /* psi(0) is a signed infinity, not a NaN -- it is a simple pole and both
     * MATLAB and scipy report the sign. The negative integers below are poles
     * too, but the reflection formula's limit has no consistent sign there. */
    if (x == 0.0)
        return copysign(INFINITY, -x);
    if (x < 0.0 && x == floor(x))
        return NAN; /* pole */
    if (x < 0.5) /* reflection: psi(1-x) - psi(x) = pi cot(pi x) */
        return dyn_digamma(1.0 - x) - M_PI / tan(M_PI * x);

    /* Recurrence up to where the asymptotic series is good. */
    while (x < 15.0) {
        r -= 1.0 / x;
        x += 1.0;
    }
    x2 = 1.0 / (x * x);
    f = log(x) - 0.5 / x;
    f -= x2 * (1.0 / 12.0 -
               x2 * (1.0 / 120.0 -
                     x2 * (1.0 / 252.0 -
                           x2 * (1.0 / 240.0 - x2 * (1.0 / 132.0)))));
    return r + f;
}

double dyn_polygamma(int n, double x)
{
    double acc = 0.0, sgn, nfact, term, xp, sum;
    int i, k;

    if (n < 0)
        return NAN;
    if (n == 0)
        return dyn_digamma(x);
    if (x <= 0.0 && x == floor(x))
        return NAN; /* pole */
    if (x < 0.0) {
        /* No reflection formula is used here: psi^(n) of a negative
         * non-integer needs the n-th derivative of pi cot(pi x), which is a
         * different expansion for every n. Reporting NAN is better than
         * reporting a number from a formula that was never checked. */
        return NAN;
    }

    sgn = ((n + 1) % 2 == 0) ? 1.0 : -1.0; /* (-1)^(n+1) */
    nfact = 1.0;
    for (i = 1; i <= n; i++)
        nfact *= (double)i;

    /* psi^(n)(x) = psi^(n)(x+1) + (-1)^(n+1) n! / x^(n+1): walk x up to where
     * the Euler-Maclaurin tail converges, accumulating the shed terms. */
    while (x < 15.0) {
        acc += sgn * nfact / pow(x, (double)(n + 1));
        x += 1.0;
    }

    /* psi^(n)(x) ~ (-1)^(n+1) [ (n-1)!/x^n + n!/(2 x^(n+1))
     *                           + sum_k B_2k (n+2k-1)! / ((2k)! x^(n+2k)) ] */
    {
        double nm1fact = nfact / (double)n; /* (n-1)! */
        sum = nm1fact / pow(x, (double)n) + nfact / (2.0 * pow(x, (double)(n + 1)));
        /* (n+2k-1)!/(2k)! built incrementally rather than as two factorials,
         * which would overflow long before the term is small. At k = 1 that is
         * (n+1)!/2!. */
        term = nfact * (double)(n + 1) / 2.0;
        xp = pow(x, (double)(n + 2));
        for (k = 1; k <= (int)(sizeof(MX_B2K) / sizeof(MX_B2K[0])); k++) {
            double add = MX_B2K[k - 1] * term / xp;
            sum += add;
            if (fabs(add) < fabs(sum) * 1.0e-17)
                break;
            /* (n+2k+1)!/(2k+2)! from (n+2k-1)!/(2k)! */
            term *= (double)(n + 2 * k) * (double)(n + 2 * k + 1) /
                    ((2.0 * k + 1.0) * (2.0 * k + 2.0));
            xp *= x * x;
        }
        return acc + sgn * sum;
    }
}

/* ====================================================================
 * Airy
 * ==================================================================== */

/* Maclaurin series. Ai = c1 f - c2 g, Bi = sqrt(3)(c1 f + c2 g), where
 *   f = sum_k 3^k (1/3)_k x^(3k)/(3k)!,  g = sum_k 3^k (2/3)_k x^(3k+1)/(3k+1)!
 * Well conditioned near the origin; at x = -7 it is cancelling terms of size
 * e^zeta against a result of order 1, which is where it hands over. */
static void mx_airy_series(double x, double *ai, double *aip, double *bi,
                           double *bip)
{
    const double C1 = 0.355028053887817239260;  /* 3^(-2/3)/Gamma(2/3) */
    const double C2 = 0.258819403792806798405;  /* 3^(-1/3)/Gamma(1/3) */
    const double SQRT3 = 1.732050807568877293527;
    double f = 1.0, g = x, fp = 0.0, gp = 1.0;
    double tf = 1.0, tg = x;
    double x3 = x * x * x;
    int k;

    /* The term ratios, derived from the definitions rather than remembered:
     *   f_k = 3^k (1/3)_k x^3k / (3k)!
     *     f_k/f_(k-1) = 3 (1/3 + k - 1) x^3 / [(3k-2)(3k-1)(3k)]
     *                 = (3k-2) x^3 / [(3k-2)(3k-1)(3k)]
     *                 = x^3 / [(3k-1)(3k)]
     *   g_k = 3^k (2/3)_k x^(3k+1) / (3k+1)!
     *     g_k/g_(k-1) = 3 (2/3 + k - 1) x^3 / [(3k-1)(3k)(3k+1)]
     *                 = (3k-1) x^3 / [(3k-1)(3k)(3k+1)]
     *                 = x^3 / [(3k)(3k+1)]
     * f' and g' are carried term by term -- d/dx x^(3k) = 3k x^(3k-1) -- which
     * is both cheaper and better conditioned than differencing two series. */
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

    if (ai)  *ai  = C1 * f - C2 * g;
    if (bi)  *bi  = SQRT3 * (C1 * f + C2 * g);
    if (aip) *aip = C1 * fp - C2 * gp;
    if (bip) *bip = SQRT3 * (C1 * fp + C2 * gp);
}

/* Oscillatory asymptotic expansion for x -> -inf. Optimal truncation error is
 * O(e^-2 zeta), which at the x = -7 switch point is 2e-11 and improves fast. */
static void mx_airy_asym_neg(double x, double *ai, double *aip, double *bi,
                             double *bip)
{
    double z = -x, zeta = (2.0 / 3.0) * pow(z, 1.5);
    double u_even = 1.0, u_odd = 0.0;   /* sum_(k even/odd) (-1)^(k/2) u_k/zeta^k */
    double v_even = 1.0, v_odd = 0.0;   /* the same with v_k */
    double u = 1.0, zp = 1.0, prev = HUGE_VAL;
    int k;

    /* DLMF 9.7.9-9.7.12. u_0 = v_0 = 1,
     *   u_k = u_(k-1) (6k-5)(6k-3)(6k-1) / ((2k-1) 216 k),  v_k = u_k (6k+1)/(1-6k).
     * The k-th term of every one of the four sums carries (-1)^(k/2) with
     * integer division -- so k = 0,1 are +, k = 2,3 are -, k = 4,5 are +, and
     * the even-k terms go to one sum and the odd-k terms to the other. Written
     * out this way rather than as a sign expression per branch, because the
     * four series differ only in WHICH sum multiplies cos and which multiplies
     * sin, and getting that wrong produces a smooth, plausible, wrong curve. */
    for (k = 1; k < 30; k++) {
        double kk = (double)k, t, v, sgn;
        u *= ((6.0 * kk - 5.0) * (6.0 * kk - 3.0) * (6.0 * kk - 1.0)) /
             ((2.0 * kk - 1.0) * 216.0 * kk);
        zp *= zeta;
        t = u / zp;
        if (fabs(t) > prev) /* asymptotic: stop at the smallest term */
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
        double p1 = 1.0 / (sqrt(M_PI) * pow(z, 0.25));   /* for Ai, Bi   */
        double p2 = pow(z, 0.25) / sqrt(M_PI);           /* for Ai', Bi' */
        if (ai)  *ai  = p1 * ( c * u_even + s * u_odd);
        if (bi)  *bi  = p1 * (-s * u_even + c * u_odd);
        if (aip) *aip = p2 * ( s * v_even - c * v_odd);
        if (bip) *bip = p2 * ( c * v_even + s * v_odd);
    }
}

void dyn_airy(double x, double *ai, double *aip, double *bi, double *bip)
{
    if (x >= 0.1) {
        /* Ai  = (1/pi) sqrt(x/3) K_1/3(zeta)
         * Ai' = -(x/(pi sqrt3)) K_2/3(zeta)
         * Bi  = sqrt(x/3) (I_-1/3(zeta) + I_1/3(zeta))
         * Bi' = (x/sqrt3) (I_-2/3(zeta) + I_2/3(zeta))
         * Every one of these is a positive combination of positive quantities,
         * so the positive half of the domain has no cancellation anywhere --
         * which is why it is handled separately from the series. */
        double zeta = (2.0 / 3.0) * pow(x, 1.5);
        const double SQRT3 = 1.732050807568877293527;
        if (ai)
            *ai = (1.0 / M_PI) * sqrt(x / 3.0) * dyn_besselk_nu(1.0 / 3.0, zeta);
        if (aip)
            *aip = -(x / (M_PI * SQRT3)) * dyn_besselk_nu(2.0 / 3.0, zeta);
        if (bi)
            *bi = sqrt(x / 3.0) * (dyn_besseli_nu(-1.0 / 3.0, zeta) +
                                   dyn_besseli_nu(1.0 / 3.0, zeta));
        if (bip)
            *bip = (x / SQRT3) * (dyn_besseli_nu(-2.0 / 3.0, zeta) +
                                  dyn_besseli_nu(2.0 / 3.0, zeta));
        return;
    }
    if (x >= -7.0) {
        mx_airy_series(x, ai, aip, bi, bip);
        return;
    }
    mx_airy_asym_neg(x, ai, aip, bi, bip);
}
