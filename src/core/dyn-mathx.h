/*
 * dyn-mathx -- special functions. PURE C: no JSValue, no JSContext. Compiles
 * with -Isrc/core alone plus libm.
 *
 * This is MATLAB tier B: the functions whose accuracy depends on WHERE in the
 * argument plane you evaluate them. Tier A (mod, nthroot, erfinv, psi, primes,
 * ...) stays in the binding, because every one of those is a one-liner over
 * libm with no regime structure worth isolating.
 *
 * WHAT "ACCURATE" MEANS HERE. Every function below switches algorithm at a
 * documented argument threshold, and the accuracy is a property of the regime,
 * not of the function. The per-regime table lives in tests/test_mathx_tierb.js
 * and is measured, not asserted -- CLAUDE.md §5 forbids claiming bit-equality
 * between two numerics backends, and 15 of 156 ML-oracle lines already differ
 * between openlibm and the macOS system libm. The thresholds are chosen so the
 * two methods OVERLAP: at the switch point both are within the stated bound,
 * so there is no cliff hiding at the seam. Where a band is genuinely worse than
 * the rest of the domain it is stated here in the header, not buried.
 *
 * Nothing here allocates and nothing here has state, so every function is
 * reentrant and thread-safe.
 */
#ifndef DYN_MATHX_H
#define DYN_MATHX_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- incomplete gamma ---------------------------------------------------
 *
 * P(a,x) is the REGULARISED lower incomplete gamma, MATLAB's gammainc(x,a) --
 * note MATLAB's argument order is (x,a) and this is (a,x), which is the order
 * every reference uses. Q = 1 - P, computed directly rather than by
 * subtraction so the upper tail keeps its relative accuracy.
 *
 * Method: the ascending series for x < a+1, the Lentz continued fraction
 * otherwise. Both converge for every a > 0, and the crossover is where each is
 * at its best. MEASURED worst case: 5.7e-16 against the closed forms P(1,x) and
 * P(1/2,x), and 2.9e-14 for Q far out in the tail where 1-P has no relative
 * accuracy at all.
 */
double dyn_gammainc_p(double a, double x);
double dyn_gammainc_q(double a, double x);

/* x such that P(a,x) = p, for 0 <= p <= 1 and a > 0. Halley from an
 * Abramowitz-Stegun starting estimate. MEASURED round trip: 5.8e-14. */
double dyn_gammaincinv(double a, double p);

/* ---- incomplete beta ---------------------------------------------------- */

/* Regularised I_x(a,b), MATLAB's betainc(x,a,b). Lentz continued fraction with
 * the standard x <-> 1-x reflection so the fraction is always evaluated where
 * it converges fastest. MEASURED: 6.5e-15 against the binomial-tail closed form
 * at integer parameters, 6.2e-15 on the symmetry identity at real ones. */
double dyn_betainc(double x, double a, double b);

/* x such that I_x(a,b) = p. Halley from a normal approximation. MEASURED round
 * trip: 1.2e-14. */
double dyn_betaincinv(double p, double a, double b);

/* ---- modified Bessel ----------------------------------------------------
 *
 * Real order nu >= 0 and real x > 0. Integer-order wrappers take an int and
 * clamp nothing -- a negative order is reflected (I_-n = I_n, K_-n = K_n).
 *
 * I_nu: ascending power series for x <= 20 (every term positive, so there is
 * no cancellation and the accuracy is the accuracy of tgamma), Hankel
 * asymptotic beyond, where the optimal-truncation floor is e^-2x < 5e-18.
 *
 * K_nu: trapezoidal quadrature of the integral representation
 *     K_nu(x) e^x = int_0^inf exp(-x (cosh t - 1)) cosh(nu t) dt
 * for x >= 0.5. The integrand is analytic in a strip of half-width pi/2 and
 * decays doubly exponentially, which is exactly the case where the trapezoid
 * rule converges geometrically -- the error is O(e^-pi^2/h), so a modest step
 * reaches machine precision. The step is scaled by 1/sqrt(x) because the peak
 * narrows as x grows. Below x = 0.5 the quadrature would need cosh t past the
 * overflow point, so the series forms are used instead.
 *
 * The scaled forms exist because K_nu(x) underflows for x > 745 while
 * K_nu(x)e^x does not, and I_nu(x) overflows for x > 713 while I_nu(x)e^-x
 * does not. Anything comparing the two (the Wronskian check in the test) must
 * use the scaled pair.
 */
double dyn_besseli_nu(double nu, double x);
double dyn_besselk_nu(double nu, double x);
double dyn_besseli_scaled(double nu, double x);  /* I_nu(x) * e^-x  */
double dyn_besselk_scaled(double nu, double x);  /* K_nu(x) * e^x   */
double dyn_besseli(int n, double x);
double dyn_besselk(int n, double x);

/* ---- ordinary Bessel ----------------------------------------------------
 *
 * Integer order only, straight through to libm's j0/j1/jn/y0/y1/yn -- which is
 * openlibm when the archive is present and the platform libm otherwise, and
 * that choice is observable (CLAUDE.md §5). Non-integer order is not offered
 * rather than offered badly. */
double dyn_besselj(int n, double x);
double dyn_bessely(int n, double x);

/* Hankel: H^(1)_n = J_n + i Y_n, H^(2)_n = J_n - i Y_n. `kind` is 1 or 2.
 * Writes the real and imaginary parts. Returns 0 on success, -1 for a bad
 * kind. */
int dyn_besselh(int kind, int n, double x, double *re, double *im);

/* ---- elliptic ----------------------------------------------------------- */

/* Complete elliptic integrals of the first and second kind, in MATLAB's
 * PARAMETER convention m = k^2 (not the modulus k, and not the modular angle).
 * Arithmetic-geometric mean: quadratic convergence, and the only special case
 * is m = 1 where K diverges. Domain m <= 1. MEASURED on Legendre's relation
 * E(m)K(1-m) + E(1-m)K(m) - K(m)K(1-m) = pi/2, which constrains K and E
 * jointly: 9.9e-16. */
double dyn_ellipk(double m);
double dyn_ellipe(double m);

/* Jacobi elliptic sn/cn/dn by descending Landen transformation. Domain
 * 0 <= m <= 1. Returns 0 on success, -1 for m out of range. MEASURED on both
 * defining identities (sn^2+cn^2 = 1, dn^2+m sn^2 = 1): 2.2e-16. */
int dyn_ellipj(double u, double m, double *sn, double *cn, double *dn);

/* ---- Legendre ----------------------------------------------------------- */

/* Associated Legendre P_n^m(x) WITH the Condon-Shortley phase, which is what
 * MATLAB's legendre() returns. 0 <= m <= n, |x| <= 1. Upward recurrence in
 * degree, which is the stable direction. NAN for arguments out of range.
 * MEASURED against the written-out closed forms through P_3 and on the degree
 * recurrence for m = 0..4, n up to 12: 9.4e-15. */
double dyn_legendre(int n, int m, double x);

/* ---- polygamma ---------------------------------------------------------- */

/* psi^(n)(x), the n-th derivative of the digamma. n = 0 is the digamma itself.
 * Recurrence up to a large argument, then the Euler-Maclaurin asymptotic;
 * MEASURED on the exact recurrence: 4.7e-15. Zero and negative integer x are
 * poles and return NAN. A negative NON-integer x also returns NAN rather than a
 * number: the reflection formula needs the n-th derivative of pi cot(pi x),
 * which is a different expansion for every n, and reporting NAN is better than
 * reporting a value from a formula nothing has checked. */
double dyn_polygamma(int n, double x);

/* Digamma alone, exported because the binding's tier-A `psi` is the same
 * function and duplicating it is how the two drift apart. */
double dyn_digamma(double x);

/* ---- Airy ---------------------------------------------------------------
 *
 * Ai, Ai', Bi, Bi'. Any out-param may be NULL.
 *
 * Three regimes. Measured worst-case relative error of the Wronskian identity
 * (tests/oracle_mathx_tierb.c, which sweeps x at 0.01 resolution through both
 * switch points):
 *
 *   x >= 0.1       via K_1/3 and I_+-1/3 of zeta = (2/3) x^3/2   6.1e-15
 *   -7 <= x < 0.1  Maclaurin series                              4.7e-12
 *   -12 <= x < -7  oscillatory asymptotic                        1.8e-14
 *   x < -12        oscillatory asymptotic                        8.7e-16
 *
 * THE WEAK REGIME IS THE SERIES, NOT THE BAND -- which is the opposite of what
 * was predicted before it was measured. Approaching x = -7 the Maclaurin series
 * cancels terms of size e^zeta against a result of order 1, and by the switch
 * point that has cost about four digits; the asymptotic side has already
 * reached 1e-14 by then, so the handover happens where the asymptotic is
 * strong and the series is at its weakest. Moving the switch point toward zero
 * would improve the series end and cost the asymptotic end, and -7 is where
 * the two curves cross.
 *
 * The positive half has no cancellation anywhere, because Ai, Bi and their
 * derivatives are there positive combinations of positive Bessel values. That
 * is why it is a separate regime rather than an extension of the series.
 */
void dyn_airy(double x, double *ai, double *aip, double *bi, double *bip);

#ifdef __cplusplus
}
#endif

#endif /* DYN_MATHX_H */
