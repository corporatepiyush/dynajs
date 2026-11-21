/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:mathx
declare module "dyna:mathx" {
    /** Mathematical constants, written with enough digits for one correctly-rounded conversion. */
    const E: number;
    const Pi: number;
    const Phi: number;
    const Sqrt2: number;
    const SqrtE: number;
    const SqrtPi: number;
    const Ln2: number;
    const Ln10: number;
    const Log2E: number;
    const Log10E: number;
    const MaxInt32: number;
    const MinInt32: number;
    const MaxSafeInteger: number;
    const MaxInt64: bigint;

    /** Smallest positive normal double (DBL_MIN). */
    function realmin(): number;
    /** Largest finite double (DBL_MAX). */
    function realmax(): number;
    /** 2^53, the largest integer every double below it represents exactly. */
    function flintmax(): number;
    /** The gap to the next double away from zero (eps() is eps(1)); eps(realmax) is 2^971. */
    function eps(x?: number): number;

    /** C99 round (ties away from zero). */
    function round(x: number): number;
    /** Round half to even. */
    function roundToEven(x: number): number;
    /** Truncate toward zero. */
    function fix(x: number): number;
    /** 1, -1, or x itself (-0 and NaN pass through). */
    function sign(x: number): number;
    /** The sign bit directly. */
    function signbit(x: number): boolean;
    /** C trunc. */
    function trunc(x: number): number;
    /** Splits into [intPart, fracPart]. */
    function modf(x: number): [number, number];

    /** MATLAB floored modulo: mod(-7, 3) is 2; mod(a, 0) is a. */
    function mod(a: number, b: number): number;
    /** Truncated fmod. */
    function rem(a: number, b: number): number;
    /** The C99 truncated remainder. */
    function fmod(a: number, b: number): number;
    /** C99 round-to-nearest remainder. */
    function remainder(a: number, b: number): number;
    /** Integer division with an explicit rounding mode: "fix"|"floor"|"ceil"|"round". */
    function idivide(a: number, b: number, mode?: "fix" | "floor" | "ceil" | "round"): number;
    /** The real n-th root; defined for negative x with odd integer n. */
    function nthroot(x: number, n: number): number;

    /** Gamma function (tgamma). */
    function gamma(x: number): number;
    function cbrt(x: number): number;
    function hypot(a: number, b: number): number;
    function copysign(a: number, b: number): number;
    /** The next representable double after `a` in the direction of `b`. */
    function nextafter(a: number, b: number): number;
    function expm1(x: number): number;
    function log1p(x: number): number;
    function log2(x: number): number;
    /** The unbiased floating-point exponent (logb). */
    function logb(x: number): number;
    /** 2^x (exp2), the inverse of log2. */
    function pow2(x: number): number;
    function deg2rad(x: number): number;
    function rad2deg(x: number): number;
    /** The smallest p with 2^p >= |x|; nextpow2(0) is 0. */
    function nextpow2(x: number): number;
    /** x * 2**n. */
    function scalbn(x: number, n: number): number;
    /** Identical to scalbn (MATLAB spelling). */
    function ldexp(frac: number, exp: number): number;
    /** Splits into x = frac * 2**exp with |frac| in [0.5, 1). */
    function frexp(x: number): [number, number];
    /** ±Inf and NaN give 2^31-1, 0 gives -(2^31), else the unbiased exponent. */
    function ilogb(x: number): number;
    /** Tests infinity, optionally restricted to one sign. */
    function isInf(x: number, sign?: number): boolean;
    function isNaN(x: number): boolean;

    function erf(x: number): number;
    function erfc(x: number): number;
    /** Inverts erf; erfinv(±1) is ±Inf. */
    function erfinv(y: number): number;
    /** erfinv(1 - y), domain [0, 2]. */
    function erfcinv(y: number): number;
    /** exp(x^2)*erfc(x), finite where erfc underflows. */
    function erfcx(x: number): number;

    /** [log|Gamma(x)|, sign of Gamma(x)] via the reentrant lgamma_r. */
    function lgamma(x: number): [number, number];
    /** log|Gamma| without the sign. */
    function gammaln(x: number): number;
    /** Computed through lgamma, so it does not overflow for moderate arguments the way `tgamma(a)*tgamma(b)/tgamma(a+b)` does. */
    function beta(a: number, b: number): number;
    /** Natural log of the beta function. */
    function betaln(a: number, b: number): number;
    /** Digamma; psi(0) is ±Inf, negative integers are NaN. */
    function psi(x: number): number;
    /** n-th derivative, order in [0, 64]; negative non-integer x by reflection, poles are NaN. */
    function polygamma(n: number, x: number): number;
    /** Regularised incomplete gamma (x first); "upper" selects the complement. */
    function gammainc(x: number, a: number, tail?: "upper"): number;
    /** Inverts P(a, x) = p. */
    function gammaincinv(p: number, a: number): number;
    /** Regularised incomplete beta. */
    function betainc(x: number, a: number, b: number): number;
    /** Inverse of the regularized incomplete beta function. */
    function betaincinv(p: number, a: number, b: number): number;
    /** E1(x), defined for x > 0. */
    function expint(x: number): number;

    /** Integer order only. |order| above 16777216 that would not underflow at x is a RangeError. */
    function besselj(n: number, x: number): number;
    /** Bessel function of the second kind, Y_n(x). */
    function bessely(n: number, x: number): number;
    /** Real order. Measured within 4 ULP of mpmath for -1 < nu <= 600 (plus nu up to 2000 grid rows), 1e-3 <= x <= 700 (tests/mathx/test_mathx_ref.js grid plus a 1500-point random sweep): below x = 20 (or while the asymptotic first term is large) the power series sums in doubles that carry each term's rounding error (fma error-free products, compensated sum; dropping the product error alone costs 5 ULP at nu = 100, x = 700) and (x/2)^nu / Gamma(nu + 1) is built from exact products (four interleaved compensated chains for Gamma, so nu = 100 costs ~0.3 us), never exp(nu ln(x/2) - lgamma(nu + 1)), whose rounded exponent alone cost |exponent| ULPs; the unscaled value is computed directly, so it no longer underflows to 0 where I_nu(x) e^-x does (besseli(500, 100) = 3.59e-283). Orders 150 <= nu <= 1e6 (and 40 <= nu < 150 when x > 20, where the series is slower) use Debye's uniform expansion (DLMF 10.41.3, U_1..U_10; truncation below 1e-16 relative from nu = 40): the exponent sqrt(nu^2 + x^2) + nu ln(x / (nu + sqrt(nu^2 + x^2))) is formed in double-double (atanh-series log, k ln2 reduction, nu^2 / (sqrt(nu^2 + x^2) + x) for x > nu so the scaled form does not cancel), so the cost is a flat ~0.25-0.3 us at any order and the 4 ULP bound holds to nu = 1e6 (tests/mathx/test_mathx_debye.c: worst 1.8 ULP against exact finite-sum oracles; above order 2000 the old logarithmic prefactor was 3749 ULP off at nu = 2651). Orders above 1e6 or below -1 keep the logarithmic prefactor. */
    function besseli(nu: number, x: number): number;
    /** Real order. Measured within 4 ULP (worst 3) of mpmath at 120 digits (mpmath's own besselk is wrong at large order below ~80 digits) on the test grid and two 2620-point sweeps (near-integer orders, orders to 600, x from 1e-300 and 1e-10 to 1000). nu = n + mu with |mu| <= 1/2: K_mu, K_mu+1 come from Temme's series for x <= 1 (1/Gamma(1 +- mu) from its Taylor series, no lgamma differences, so near-integer orders do not cancel; beyond x = 1 the series loses a ULP) or else from the trapezoid rule on K = int e^(-x cosh t) cosh(mu t) dt (power-of-two step <= 1/(2 sqrt x); for this analytic, doubly-exponentially decaying integrand the error falls exponentially with 1/step, below 1e-20 at that step), then forward recurrence to n in double-double with exponent tracking, so the unscaled value neither overflows early nor loses the e^-x factor's bits. Orders nu >= 40 skip the O(nu) recurrence and use the same Debye uniform expansion as besseli (signs (-1)^k U_k), a flat ~0.25-0.3 us at any order. Orders above 1e6 (and non-finite) answer +Infinity -- the limit of K_nu(x) as nu grows, for any finite x > 0; kept from the recurrence era, although the Debye branch itself has no order-dependent cost. */
    function besselk(nu: number, x: number): number;
    /** I_nu(x) e^-x; same method and 4 ULP bound as besseli. */
    function besseliScaled(nu: number, x: number): number;
    /** K_nu(x) e^x; same method and 4 ULP bound as besselk; the 1e6 order clamp (+Infinity above it) applies here too. */
    function besselkScaled(nu: number, x: number): number;
    /** Hankel function J_n ± i Y_n as [re, im]; kind 1|2; the order must be an int32. */
    function besselh(n: number, x: number, kind: 1 | 2): [number, number];

    /** Complete elliptic integrals [K, E] from one AGM iteration. */
    function ellipke(m: number): [number, number];
    /** Jacobi elliptic functions {sn, cn, dn}. */
    function ellipj(u: number, m: number): { sn: number; cn: number; dn: number };

    /** Associated Legendre functions P_n^m for the whole column m = 0..n. */
    function legendre(n: number, x: number): number[];
    /** The single value P_n^m(x); degree capped at 150. */
    function legendreP(n: number, m: number, x: number): number;
    /** All four Airy values from one evaluation, each within 4 ULP of mpmath on [-30, 30] (of the envelope |x|^(-+1/4)/sqrt(pi) where the functions oscillate, x < -1): the Maclaurin series on [-12, 8] in doubles carrying each term's rounding error (fma error-free products, compensated sums: it cancels by up to 6 digits near -7 and 13 near 8), the asymptotic forms with a double-double phase zeta - pi/4 below -12 and above 10, K_{1/3}, K_{4/3} from one cancellation-free quadrature (K_{2/3} = K_{4/3} - 2/(3 zeta) K_{1/3}) for Ai and Ai' on (8, 10). */
    function airy(x: number): { ai: number; aip: number; bi: number; bip: number };

    /** Deterministic Miller-Rabin with a 12-witness set; proven for every uint64. */
    function isPrime(n: number): boolean;
    /** Ascending prime factors with multiplicity; factor(1) is []. */
    function factor(n: number): number[];
    /** Every prime <= n by sieve, up to 5e7. */
    function primes(n: number): number[];

    /** Both arguments within the int64 range; wider BigInts are refused. */
    function gcd(a: number | bigint, b: number | bigint): bigint;
    /** Both arguments within the int64 range; wider BigInts are refused. */
    function lcm(a: number | bigint, b: number | bigint): bigint;
    /** n! exactly, capped at 10000. */
    function factorial(n: number): bigint;
    /** The magnitude as an unsigned BigInt; magnitudes of 2^64 or more are refused, not truncated. */
    function abs(n: bigint): bigint;
    /** Minimum bits to represent the magnitude (bitLen(0n) is 0); magnitudes of 2^64 or more are refused. */
    function bitLen(n: bigint): number;
    /** Set bits in the magnitude; magnitudes of 2^64 or more are refused. */
    function popcount(n: bigint): number;

    /** Binomial coefficient built multiplicatively in min(k, n-k) steps; huge orders return Infinity promptly, never hang. */
    function nchoosek(n: number, k: number): number;
    /** Every permutation, reverse lexicographic, at most 8 elements. */
    function perms(v: number[]): number[][];
    /** Rational approximation by continued fractions within relative tolerance. */
    function rat(x: number, tol?: number): [number, number];

    /** n points INCLUSIVE of both ends (the last is exactly b); Number.range is the exclusive-end stepper. */
    function linspace(a: number, b: number, n?: number): number[];
    /** 10^t over the linspace grid. */
    function logspace(a: number, b: number, n?: number): number[];
    /** Array-like inputs are capped at 16777216 elements (RangeError beyond). */
    function cumsum(v: number[]): number[];
    /** The running product. */
    function cumprod(v: number[]): number[];
    /** Adjacent differences, one element shorter; array-like inputs capped at 16777216 elements (RangeError beyond). */
    function diff(v: number[]): number[];
    /** Solves A x = b for symmetric positive-definite A (row-major n x n Float64Array) by Cholesky; a non-positive pivot or asymmetric A is a RangeError. */
    function solveSPD(A: Float64Array, b: Float64Array): Float64Array;

    /** Width-parameterised fixed-width bit primitives. */
    namespace bits {
        const uintSize: number;
        function leadingZeros8(x: number): number;
        function leadingZeros16(x: number): number;
        function leadingZeros32(x: number): number;
        function leadingZeros64(x: bigint): number;
        function trailingZeros8(x: number): number;
        function trailingZeros16(x: number): number;
        function trailingZeros32(x: number): number;
        function trailingZeros64(x: bigint): number;
        function onesCount8(x: number): number;
        function onesCount16(x: number): number;
        function onesCount32(x: number): number;
        function onesCount64(x: bigint): number;
        function len8(x: number): number;
        function len16(x: number): number;
        function len32(x: number): number;
        function len64(x: bigint): number;
        function reverse8(x: number): number;
        function reverse16(x: number): number;
        function reverse32(x: number): number;
        function reverse64(x: bigint): bigint;
        function reverseBytes16(x: number): number;
        function reverseBytes32(x: number): number;
        function reverseBytes64(x: bigint): bigint;
        function rotateLeft8(x: number, k: number): number;
        function rotateLeft16(x: number, k: number): number;
        function rotateLeft32(x: number, k: number): number;
        function rotateLeft64(x: bigint, k: number): bigint;
        /** Rotate right; k reduces modulo the width, so a negative k rotates left. */
        function rotateRight8(x: number, k: number): number;
        function rotateRight16(x: number, k: number): number;
        function rotateRight32(x: number, k: number): number;
        function rotateRight64(x: bigint, k: number): bigint;
        /** Widening add with carry in and out. */
        function add32(a: number, b: number, carry: number): [number, number];
        function add64(a: bigint, b: bigint, carry: bigint): [bigint, bigint];
        function sub32(a: number, b: number, borrow: number): [number, number];
        function sub64(a: bigint, b: bigint, borrow: bigint): [bigint, bigint];
        /** Full-width product, high word first. */
        function mul32(a: number, b: number): [number, number];
        function mul64(a: bigint, b: bigint): [bigint, bigint];
        /** Divides the double-width hi:lo by y; throws on y==0 or y<=hi. */
        function div32(hi: number, lo: number, y: number): [number, number];
        function div64(hi: bigint, lo: bigint, y: bigint): [bigint, bigint];
        function rem32(hi: number, lo: number, y: number): number;
        function rem64(hi: bigint, lo: bigint, y: bigint): bigint;
    }

    /** Reductions over a Float64Array (fast path) or any array-like (capped at 16777216 elements). sum is Neumaier-compensated; median/quantile use R-7.
     *  Unlike DataFrame: empty input and zero variance THROW RangeError and a NaN element answers NaN; min/max follow Math.min/Math.max. */
    namespace stats {
        /** Compensated sum; empty array sums to +0. */
        function sum(a: ArrayLike<number>): number;
        /** sum / n; the empty array is 0/0 = NaN. */
        function mean(a: ArrayLike<number>): number;
        /** Two-pass variance, sample (n-1) by default; opts.pop divides by n. */
        function variance(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** sqrt(variance) under the same opts and gates. */
        function stddev(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** quantile(a, 0.5); empty throws RangeError. */
        function median(a: ArrayLike<number>): number;
        /** R-7 linear quantile (DataFrame.QUANTILE's convention); q in [0, 1] and a non-empty array, else RangeError. */
        function quantile(a: ArrayLike<number>, q: number): number;
        /** Math.min semantics: NaN poisons; empty is +Infinity. */
        function min(a: ArrayLike<number>): number;
        /** Math.max semantics: NaN poisons; empty is -Infinity. */
        function max(a: ArrayLike<number>): number;
        /** Sample covariance (n-1); opts.pop divides by n; a length mismatch throws RangeError. */
        function cov(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** Pearson r clamped to [-1, 1]; a length mismatch or zero variance throws RangeError. */
        function corr(a: ArrayLike<number>, b: ArrayLike<number>): number;

        /** Distributions (scipy.stats semantics; values match scipy 1.18 to ~1e-9 relative, see tests/mathx/test_mathx_stats_ref.js). Each
         *  takes x as a number (returns a number) or an array-like (returns a fresh Float64Array, one value per element). Out-of-domain
         *  parameters (df <= 0, p outside [0, 1], non-integer n) give NaN rather than throwing. Tails are computed directly, never as
         *  1 - cdf, so tSf(9, 250) keeps its 2.9e-17. */
        function normPdf(x: number): number;
        function normPdf(x: ArrayLike<number>): Float64Array;
        function normCdf(x: number): number;
        function normCdf(x: ArrayLike<number>): Float64Array;
        /** Upper tail 1 - normCdf(x) without cancellation. */
        function normSf(x: number): number;
        function normSf(x: ArrayLike<number>): Float64Array;
        /** Inverse normCdf (Wichura AS241, ~1e-16 relative); 0 -> -Infinity, 1 -> Infinity. */
        function normPpf(p: number): number;
        function normPpf(p: ArrayLike<number>): Float64Array;
        /** Student t with `df` degrees of freedom (df may be fractional; Infinity is the normal). */
        function tPdf(x: number, df: number): number;
        function tPdf(x: ArrayLike<number>, df: number): Float64Array;
        function tCdf(x: number, df: number): number;
        function tCdf(x: ArrayLike<number>, df: number): Float64Array;
        function tSf(x: number, df: number): number;
        function tSf(x: ArrayLike<number>, df: number): Float64Array;
        /** Inverse tCdf (incomplete-beta inverse polished by Newton steps). */
        function tPpf(p: number, df: number): number;
        function tPpf(p: ArrayLike<number>, df: number): Float64Array;
        /** Chi-square with `k` degrees of freedom. */
        function chi2Pdf(x: number, k: number): number;
        function chi2Pdf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Cdf(x: number, k: number): number;
        function chi2Cdf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Sf(x: number, k: number): number;
        function chi2Sf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Ppf(p: number, k: number): number;
        function chi2Ppf(p: ArrayLike<number>, k: number): Float64Array;
        /** Fisher-Snedecor F with (d1, d2) degrees of freedom. */
        function fCdf(x: number, d1: number, d2: number): number;
        function fCdf(x: ArrayLike<number>, d1: number, d2: number): Float64Array;
        function fSf(x: number, d1: number, d2: number): number;
        function fSf(x: ArrayLike<number>, d1: number, d2: number): Float64Array;
        function fPpf(p: number, d1: number, d2: number): number;
        function fPpf(p: ArrayLike<number>, d1: number, d2: number): Float64Array;
        /** Binomial(n, p) at k successes; cdf/sf floor k, pmf of a non-integer k is 0; sf(k) = P(X > k). */
        function binomPmf(k: number, n: number, p: number): number;
        function binomPmf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        function binomCdf(k: number, n: number, p: number): number;
        function binomCdf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        function binomSf(k: number, n: number, p: number): number;
        function binomSf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        /** Poisson(mu) at k events, same k conventions as the binomial. */
        function poissonPmf(k: number, mu: number): number;
        function poissonPmf(k: ArrayLike<number>, mu: number): Float64Array;
        function poissonCdf(k: number, mu: number): number;
        function poissonCdf(k: ArrayLike<number>, mu: number): Float64Array;
        function poissonSf(k: number, mu: number): number;
        function poissonSf(k: ArrayLike<number>, mu: number): Float64Array;
        /** Survival function of the Kolmogorov limit distribution (scipy.special.kolmogorov / kstwobign.sf). */
        function kolmogorovSf(x: number): number;
        function kolmogorovSf(x: ArrayLike<number>): Float64Array;

        /** Hypothesis tests (scipy.stats semantics and defaults). Samples are array-likes of finite numbers: a NaN element, an empty
         *  sample where one is needed, or a length mismatch for paired tests throws RangeError; an unknown option key throws TypeError
         *  and an unknown enum value RangeError naming it. Samples with fewer than 2 values give NaN statistic and pvalue (as scipy).
         *  ttest1samp: one-sample t test of mean(a) against `popmean` (scipy.stats.ttest_1samp); df = n - 1. */
        function ttest1samp(a: ArrayLike<number>, popmean: number, opts?: { alternative?: StatsAlternative }): StatsTestResult & { df: number };
        /** Two-sample t test of mean(a) - mean(b) (scipy.stats.ttest_ind). equalVar (default true) pools the variances with
         *  df = na + nb - 2; equalVar: false is Welch's test with Welch-Satterthwaite df. */
        function ttestInd(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; equalVar?: boolean }): StatsTestResult & { df: number };
        /** Paired t test on a[i] - b[i] (scipy.stats.ttest_rel). */
        function ttestRel(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult & { df: number };
        /** Pooled two-proportion z test of c1/n1 - c2/n2 (statsmodels proportions_ztest with value 0). A count outside [0, total] or a
         *  non-positive total throws RangeError. */
        function proportionsZTest(c1: number, n1: number, c2: number, n2: number, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Mann-Whitney U (scipy.stats.mannwhitneyu): statistic is U1 = number of (a, b) pairs with a > b (ties 1/2), average ranks.
         *  method 'auto' is exact (Loeffler's recurrence) when there are no ties, min(na, nb) <= 8 and na * nb <= 16384, otherwise the
         *  tie-corrected normal approximation with the 0.5 continuity correction (continuity: false drops it). An explicit 'exact'
         *  accepts na * nb <= 131072 (RangeError above) and assumes no ties. */
        function mannWhitneyU(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; method?: StatsMethod; continuity?: boolean }): StatsTestResult;
        /** Two-sample Kolmogorov-Smirnov (scipy.stats.ks_2samp): statistic is the largest ECDF gap (the signed one for less/greater).
         *  'auto' is exact (Hodges' lattice-path count) when max(na, nb) <= 10000 and falls back to asymptotic if the count overflows.
         *  'asymptotic' two-sided uses the Kolmogorov limit kolmogorovSf(sqrt(en) * D), en = na nb / (na + nb) -- scipy uses the
         *  finite-n kstwo there; one-sided uses Hodges' approximation exactly as scipy. Exact p assumes no ties. */
        function ks2Samp(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; method?: StatsMethod }): StatsTestResult;
        /** Pearson correlation r with the exact beta-distribution p (scipy.stats.pearsonr); a constant input gives NaN r and p; n = 2
         *  gives p 1 (two-sided) or 0.5. */
        function pearson(x: ArrayLike<number>, y: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Spearman rho (Pearson r of average ranks) with the t-distribution p on n - 2 df (scipy.stats.spearmanr). */
        function spearman(x: ArrayLike<number>, y: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Pearson chi-square goodness of fit (scipy.stats.chisquare); `expected` defaults to the uniform mean and must have the same
         *  total as `observed` (1e-8 relative, else RangeError); df = n - 1 - ddof. */
        function chiSquare(observed: ArrayLike<number>, expected?: ArrayLike<number> | null, opts?: { ddof?: number }): StatsTestResult & { df: number };
        /** Chi-square test of independence on an r x c table of rows (scipy.stats.chi2_contingency): Yates' correction (default on) applies
         *  only when df = 1; df = 0 gives statistic 0 and p 1. A negative cell, a ragged table or a zero row/column total throws RangeError.
         *  `expected` is the table of expected frequencies, one Float64Array per row. */
        function chi2Contingency(table: ArrayLike<ArrayLike<number>>, opts?: { correction?: boolean }): StatsTestResult & { df: number; expected: Float64Array[] };
        /** Ranks 1..n (scipy.stats.rankdata): 'average' (default), 'min', 'max', 'dense', or 'ordinal' (stable order of appearance).
         *  Values compare numerically, so -0 and +0 are equal: they tie, and 'ordinal' keeps their input order at every length. */
        function rankData(a: ArrayLike<number>, opts?: { method?: "average" | "min" | "max" | "dense" | "ordinal" }): Float64Array;
        /** Multiple-comparison adjusted p-values in input order: 'bonferroni', 'holm' (step-down), 'hochberg' (step-up), 'bh'
         *  (Benjamini-Hochberg, default) or 'by' (Benjamini-Yekutieli), each capped at 1 and monotone in the raw p (as
         *  statsmodels multipletests / scipy false_discovery_control). A p outside [0, 1] throws RangeError. */
        function pAdjust(pvalues: ArrayLike<number>, method?: "bonferroni" | "holm" | "hochberg" | "bh" | "by"): Float64Array;
        /** Population stability index between two samples: `bins` (default 10, 2..65536) quantile bins whose edges are
         *  sorted(expected)[floor((k + 1) n / bins)], a value v falling in the bin counting the edges <= v; then psiCounts. */
        function psi(expected: ArrayLike<number>, actual: ArrayLike<number>, opts?: { bins?: number; eps?: number }): number;
        /** PSI from per-bin counts: sum (pa - pe) ln(pa / pe) with each share floored at eps (default 1e-4). */
        function psiCounts(expected: ArrayLike<number>, actual: ArrayLike<number>, opts?: { eps?: number }): number;
        /** Percentile bootstrap confidence interval of mean or median(a), or with opts.b of stat(a) - stat(b) ('diff', default) or
         *  stat(a) / stat(b) ('ratio'). Resample r draws its indices from splitmix64 started at splitmix64(seed + r * 0x9E3779B97F4A7C15)
         *  (index = floor(draw * n / 2^64), a's n draws then b's), so the answer is a pure function of the inputs and seed (default 0)
         *  and identical for any thread count. low/high interpolate the sorted replicates linearly (numpy's default percentile) at
         *  (1 -+ level) / 2; a NaN replicate (0/0 ratio) makes both NaN. Resamples run on the worker pool off the JS thread above
         *  ~256K draws; the Promise rejects on bad input (RangeError/TypeError) and with InternalError "interrupted" at shutdown. */
        function bootstrap(a: ArrayLike<number>, opts?: { b?: ArrayLike<number>; statistic?: "mean" | "median"; compare?: "diff" | "ratio"; resamples?: number; level?: number; seed?: number }): Promise<{ estimate: number; low: number; high: number }>;
    }
    /** Tail of a stats hypothesis test: 'two-sided' (default), 'less' or 'greater' (the alternative that a's location is below/above b's). */
    type StatsAlternative = "two-sided" | "less" | "greater";
    /** How a stats test computes its p-value: 'auto' (default), 'exact' or 'asymptotic'. */
    type StatsMethod = "auto" | "exact" | "asymptotic";
    /** Statistic and p-value of a stats hypothesis test. */
    interface StatsTestResult {
        statistic: number;
        pvalue: number;
    }

    /** Signal processing with numpy.fft / scipy.signal semantics; values match numpy 2.5 / scipy 1.18 to ~1e-9 relative (see
     *  tests/mathx/test_mathx_signal_ref.js). Every input is a Float64Array (fast path) or any array-like of numbers capped at
     *  16777216 elements, never modified; every array result is a fresh Float64Array. Frequencies take an optional fs (default 2, so
     *  cutoffs are fractions of Nyquist as in scipy) unless stated. Transforms of any length are exact-size: lengths factoring into
     *  2, 3, 4 and odd primes <= 31 run a Stockham mixed-radix FFT, other lengths Bluestein's chirp-z over a power of two; plans
     *  (twiddles) are cached per size, shared across threads. Bad arguments throw TypeError (wrong type, unknown option key) or
     *  RangeError (out of range); nothing returns NaN for a bad shape. */
    namespace signal {
        /** Complex DFT X[k] = sum x[t] e^{-2 pi i k t / n}; im defaults to zeros. Lengths of re and im must match. */
        function fft(re: ArrayLike<number>, im?: ArrayLike<number>): { re: Float64Array; im: Float64Array };
        /** Inverse DFT with the 1/n factor, so ifft(fft(x)) == x. */
        function ifft(re: ArrayLike<number>, im: ArrayLike<number>): { re: Float64Array; im: Float64Array };
        /** The n/2 + 1 non-negative-frequency bins of a real signal's DFT (numpy.fft.rfft); n (default x.length) zero-pads or
         *  truncates. The DC bin's imaginary part is exactly 0, and the Nyquist bin's too for even n. */
        function rfft(x: ArrayLike<number>, n?: number): { re: Float64Array; im: Float64Array };
        /** Inverse of rfft to n real samples (default 2 * (re.length - 1); a one-bin spectrum needs n). Like numpy, the
         *  imaginary parts of DC and of the Nyquist bin of an even n are ignored, and the spectrum is cropped or zero-padded to
         *  n/2 + 1 bins. */
        function irfft(re: ArrayLike<number>, im: ArrayLike<number>, n?: number): Float64Array;
        /** Bin centres of an n-point fft for sample spacing d (default 1): [0, 1, ..., -2, -1] / (n d). */
        function fftfreq(n: number, d?: number): Float64Array;
        /** Bin centres of an n-point rfft: [0, 1, ..., floor(n/2)] / (n d). */
        function rfftfreq(n: number, d?: number): Float64Array;
        /** scipy.signal.get_window: periodic (DFT-even, the spectral-analysis default) unless opts.sym. 'kaiser' needs
         *  opts.beta >= 0. A 1-point window is [1]. */
        function getWindow(name: SignalWindow, n: number, opts?: { sym?: boolean; beta?: number }): Float64Array;
        /** Kaiser beta for a stopband attenuation in dB (scipy.signal.kaiser_beta). */
        function kaiserBeta(attenuationDb: number): number;
        /** Kaiser FIR design rule (scipy.signal.kaiserord): taps and beta reaching rippleDb (>= 8) with transition width as a
         *  fraction of Nyquist in (0, 1]. */
        function kaiserOrd(rippleDb: number, width: number): { numtaps: number; beta: number };
        /** Windowed-sinc FIR (scipy.signal.firwin): cutoff is one frequency or increasing band edges in (0, fs/2); passZero
         *  (default true) makes the first band a passband. window defaults to 'hamming'; scale (default true) gives unit gain at
         *  the centre of the first passband. A filter that passes Nyquist needs an odd numtaps (RangeError otherwise). */
        function firwin(numtaps: number, cutoff: number | ArrayLike<number>, opts?: { window?: SignalWindow; beta?: number; passZero?: boolean; scale?: boolean; fs?: number }): Float64Array;
        /** Direct-form II transposed IIR/FIR filter (scipy.signal.lfilter) from zero initial state, normalised by a[0] (which
         *  must be finite and non-zero). */
        function lfilter(b: ArrayLike<number>, a: ArrayLike<number>, x: ArrayLike<number>): Float64Array;
        /** Cascade of second-order sections (scipy.signal.sosfilt) from zero state. sos is flat (6 per section) or rows
         *  [b0, b1, b2, a0, a1, a2]; each a0 must be non-zero. Numerically safer than lfilter for high orders. */
        function sosfilt(sos: ArrayLike<number> | ArrayLike<ArrayLike<number>>, x: ArrayLike<number>): Float64Array;
        /** One RBJ Audio-EQ-Cookbook section [b0, b1, b2, 1, a1, a2] centred at f0 (0 < f0 < fs/2) with quality q (default
         *  SQRT1_2) and, for peaking and the shelves, gainDb (default 0; shelves use the Q form of alpha). Feed it to sosfilt. */
        function biquad(type: "lowpass" | "highpass" | "bandpass" | "notch" | "allpass" | "peaking" | "lowshelf" | "highshelf", f0: number, opts?: { q?: number; fs?: number; gainDb?: number }): Float64Array;
        /** Digital Butterworth design as flat second-order sections (scipy.signal.butter(..., output='sos'), including its
         *  'nearest' pole-zero pairing and section order): order 1..40, wn a cutoff for 'lowpass' (default) / 'highpass' or
         *  [low, high] for 'bandpass' / 'bandstop', strictly inside (0, fs/2). */
        function butter(order: number, wn: number | ArrayLike<number>, opts?: { btype?: "lowpass" | "highpass" | "bandpass" | "bandstop"; fs?: number }): Float64Array;
        /** Frequency response of b / a (a defaults to [1]) at n (default 512) points w = pi k / n, k < n; with opts.fs, w is
         *  reported in the units of fs (0 .. fs/2) instead of radians/sample. */
        function freqz(b: ArrayLike<number>, a?: ArrayLike<number> | null, opts?: { n?: number; fs?: number }): { w: Float64Array; re: Float64Array; im: Float64Array };
        /** freqz of a second-order-section cascade (same sos forms as sosfilt). */
        function sosfreqz(sos: ArrayLike<number> | ArrayLike<ArrayLike<number>>, opts?: { n?: number; fs?: number }): { w: Float64Array; re: Float64Array; im: Float64Array };
        /** Linear convolution (scipy.signal.convolve): 'full' (default, length na+nb-1), 'same' (centred, length na) or 'valid'
         *  (|na-nb|+1). Short kernels sum directly; large ones go through a 5-smooth rfft, agreeing to rounding. */
        function convolve(a: ArrayLike<number>, b: ArrayLike<number>, mode?: "full" | "same" | "valid"): Float64Array;
        /** Cross-correlation (scipy.signal.correlate): convolve(a, reversed b, mode). */
        function correlate(a: ArrayLike<number>, b: ArrayLike<number>, mode?: "full" | "same" | "valid"): Float64Array;
        /** Welch power spectral density (scipy.signal.welch, one-sided): segments of nperseg (default 256, clamped to x.length)
         *  overlapping by noverlap (default nperseg/2), periodic window (default 'hann'), each mean-removed unless detrend is
         *  false, zero-padded to nfft (default nperseg). scaling 'density' (V^2/Hz, default) or 'spectrum' (V^2). Segments are
         *  summed in fixed groups on the worker pool and reduced in order, so the result is identical for any thread count. */
        function welch(x: ArrayLike<number>, opts?: { fs?: number; window?: SignalWindow; beta?: number; nperseg?: number; noverlap?: number; nfft?: number; detrend?: "constant" | false; scaling?: "density" | "spectrum" }): { f: Float64Array; pxx: Float64Array };
        /** Analytic signal x + i H[x] (scipy.signal.hilbert) via the FFT; n (default x.length) zero-pads or truncates. The
         *  envelope is hypot(re, im). */
        function hilbert(x: ArrayLike<number>, n?: number): { re: Float64Array; im: Float64Array };
        /** One DFT bin by the Goertzel recurrence: sum x[t] e^{-2 pi i freq t / fs} (fs default 1). At freq = k fs / n this is
         *  fft(x) bin k; any real freq is allowed. O(n), no allocation. */
        function goertzel(x: ArrayLike<number>, freq: number, fs?: number): { re: number; im: number };
        /** Box Least Squares transit periodogram (Kovacs, Zucker & Mazeh 2002, binned form). y is mean-subtracted, then for each
         *  trial period the observations are phase-folded (t mod period, negative t wrapped into [0, period)) into opts.bins
         *  (default 400, integer in [2, 1048576]) equal phase bins; for each duration a box of max(1, round(duration / period *
         *  bins)) consecutive bins (wrapping the phase) slides over every start bin, and a box holding n of the N points with
         *  y-sum s scores s^2 / (N r (1 - r)), r = n / N. Boxes with fewer than opts.minPoints (default 5) points, holding every
         *  point, or as wide as the whole bin ring (duration close to or above the period) are skipped. Per period the
         *  highest-scoring box gives power, its duration, depth (-s / n: the drop below the mean of y) and epoch (box centre,
         *  mid-transit time folded into [0, period)); a period with no admissible box has power 0 and NaN duration/depth/epoch.
         *  best is the first period of maximal power, with sde = (best.power - mean(power)) / sample stddev(power) (NaN for one
         *  period or a flat periodogram); with no admissible box anywhere best.index is -1, best.power 0 and its other fields
         *  NaN. t need not be sorted (result independent of order up to rounding). Periods run in parallel on the worker pool;
         *  each period's result is deterministic. Cost O(periods * (N + durations * bins)). TypeError for a non-array input,
         *  an unknown option or a non-number option; RangeError when t and y lengths differ, there are fewer than
         *  max(2, minPoints) points, a t or y value is not finite, periods or durations is empty or holds a value that is not
         *  finite and > 0, or bins/minPoints is out of range. */
        function bls(t: ArrayLike<number>, y: ArrayLike<number>, periods: ArrayLike<number>, durations: ArrayLike<number>, opts?: { bins?: number; minPoints?: number }): { power: Float64Array; duration: Float64Array; depth: Float64Array; epoch: Float64Array; best: { index: number; period: number; duration: number; depth: number; epoch: number; power: number; sde: number } };
    }
    /** Geodesy on the sphere and the WGS84 ellipsoid. Angles are degrees (latitude in [-90, 90], longitude any finite value,
     *  results normalised to [-180, 180)), distances metres. Sphere functions use opts.radius (default 6371008.8 m, the IUGG
     *  mean radius); vincenty/vincentyDirect use WGS84 (a = 6378137, f = 1/298.257223563) and agree with a numerically
     *  integrated geodesic to 1 mm and 2e-7 deg (tests/mathx/test_mathx_geo_ref.js). Polygon rings are parallel latitude and
     *  longitude arrays, implicitly closed. Bad arguments throw TypeError (wrong type, unknown option) or RangeError. */
    namespace geo {
        /** Great-circle distance by the haversine formula. With lat2/lon2 arrays (same length) returns one distance per point as a
         *  Float64Array, computed on the worker pool for large inputs and identical to the scalar form element by element. */
        function haversine(lat1: number, lon1: number, lat2: number, lon2: number, opts?: { radius?: number }): number;
        function haversine(lat1: number, lon1: number, lat2: ArrayLike<number>, lon2: ArrayLike<number>, opts?: { radius?: number }): Float64Array;
        /** Radius search by brute-force haversine scan: the points of lat/lon within `meters` of q (distance <= meters), as
         *  ascending indices and their distances. The distances are bitwise equal to haversine(q.lat, q.lon, lat, lon) at those
         *  indices; most far points are rejected before the atan2 by comparing the haversine term itself. Runs on the worker pool
         *  for large inputs. meters must be finite and >= 0. */
        function haversineWithin(lat: ArrayLike<number>, lon: ArrayLike<number>, q: { lat: number; lon: number }, meters: number,
            opts?: { radius?: number }): { indices: Uint32Array; distances: Float64Array };
        /** Initial great-circle bearing from point 1 to point 2, degrees clockwise from north in [0, 360). */
        function bearing(lat1: number, lon1: number, lat2: number, lon2: number): number;
        /** Great-circle midpoint (normalised vector sum); for antipodal points the result is not unique. */
        function midpoint(lat1: number, lon1: number, lat2: number, lon2: number): { lat: number; lon: number };
        /** Point reached by travelling distance along the great circle leaving (lat, lon) on bearing. */
        function destination(lat: number, lon: number, bearing: number, distance: number, opts?: { radius?: number }): { lat: number; lon: number };
        /** Signed distance of (lat, lon) from the great circle through 1 then 2 (positive to the right of the direction of travel),
         *  and the distance from point 1 to the foot of the perpendicular along that circle (negative behind point 1). Throws
         *  RangeError when points 1 and 2 coincide or are antipodal. */
        function crossTrack(lat: number, lon: number, lat1: number, lon1: number, lat2: number, lon2: number, opts?: { radius?: number }): { crossTrack: number; alongTrack: number };
        /** Vincenty's inverse problem on WGS84: geodesic distance, the azimuth leaving point 1 and the forward azimuth arriving at
         *  point 2 (both [0, 360)). Coincident points give 0. Nearly antipodal pairs, where the iteration does not converge, throw
         *  RangeError rather than return a wrong answer. */
        function vincenty(lat1: number, lon1: number, lat2: number, lon2: number): { distance: number; azimuth1: number; azimuth2: number };
        /** Vincenty's direct problem on WGS84: the end point and forward azimuth after distance (0 .. 2.1e7 m) on azimuth. */
        function vincentyDirect(lat: number, lon: number, azimuth: number, distance: number): { lat: number; lon: number; azimuth: number };
        /** Base-32 geohash of a point with lon in [-180, 180], precision 1..12 characters (default 9). */
        function geohashEncode(lat: number, lon: number, precision?: number): string;
        /** Centre of a geohash cell and its half-sizes; case-insensitive; invalid characters throw RangeError. */
        function geohashDecode(hash: string): { lat: number; lon: number; latErr: number; lonErr: number };
        /** The eight same-precision neighbour cells; longitude wraps at the antimeridian, and a neighbour beyond a pole is null. */
        function geohashNeighbors(hash: string): { n: string | null; ne: string | null; e: string | null; se: string | null; s: string | null; sw: string | null; w: string | null; nw: string | null };
        /** Even-odd test of a point against a ring in the plane of (lon, lat) degrees (no antimeridian handling); points exactly on
         *  an edge may fall either way. With lat/lon arrays returns a Uint8Array of 0/1, computed on the worker pool when large. */
        function pointInPolygon(lat: number, lon: number, ringLat: ArrayLike<number>, ringLon: ArrayLike<number>): boolean;
        function pointInPolygon(lat: ArrayLike<number>, lon: ArrayLike<number>, ringLat: ArrayLike<number>, ringLon: ArrayLike<number>): Uint8Array;
        /** Area enclosed by a ring of great-circle edges on the sphere (exact spherical excess), whatever the orientation; of the
         *  two regions a ring bounds, the smaller one. */
        function polygonArea(ringLat: ArrayLike<number>, ringLon: ArrayLike<number>, opts?: { radius?: number }): number;
    }

    /** Element type of an NDArray. The codes are fixed for good (dyn-lak.h DYN_ND_*; never renumbered): float64, float32,
     *  float16, bfloat16, int32, int16, int8, uint8, bool. The backing typed array per dtype: Float64Array, Float32Array,
     *  Float16Array, Uint16Array (raw bfloat16 bits), Int32Array, Int16Array, Int8Array, Uint8Array, Uint8Array (0/1).
     *  Conversions between dtypes (astype, set, fill, assign, from) go through float64: to float32/float16/bfloat16 they round
     *  to nearest even (overflow gives +-Infinity, NaN stays NaN); to an integer type they truncate toward zero and saturate at
     *  the type's range, and NaN becomes 0; to bool any non-zero non-NaN value is 1. Block-quantized weights (q8_0, q4_0,
     *  q4_K) are not NDArray dtypes (dyna:nn owns them). Arithmetic kernels (later linalg functions) accept float64/float32. */
    type NDType = "float64" | "float32" | "float16" | "bfloat16" | "int32" | "int16" | "int8" | "uint8" | "bool";
    /** The typed array that backs an NDArray of each dtype. */
    type NDData = Float64Array | Float32Array | Float16Array | Uint16Array | Int32Array | Int16Array | Int8Array | Uint8Array;
    /** One axis of NDArray.slice: an integer picks one index and drops the axis (negative counts from the end); null or
     *  undefined keeps the whole axis; "start:stop:step" or [start, stop, step] (any part omitted or null) is a Python slice
     *  with Python's clamping, negative steps included. A step of 0 is a RangeError, a malformed string a SyntaxError. */
    type NDSlice = number | null | undefined | string | [number?, number?, number?];
    /** The strided n-dimensional array shared by mathx.linalg and dyna:nn (one type, no separate Tensor). A view is a backing
     *  typed array plus offset, shape and strides counted in ELEMENTS; a stride may be negative (a reversed axis) or 0 (a
     *  broadcast axis). At most 8 dimensions; a 0-dimensional array holds one element. Views share memory: slice, transpose,
     *  T, broadcastTo, expandDims, squeeze and (when the strides allow) reshape never copy, so a write through one view is seen
     *  by every view of the same buffer. Every access resolves the buffer afresh and never reads out of bounds: a detached
     *  ArrayBuffer, or a fixed-length typed array left out of bounds by a resize, is a TypeError; a view that no longer fits
     *  its (length-tracking) buffer is a RangeError. An NDArray built over an ArrayBuffer tracks a resizable buffer's length; writes into an immutable ArrayBuffer throw TypeError. Construction and the static makers are on the loop
     *  thread and cheap; nothing here blocks on I/O. The class is the same object as mathx.linalg.NDArray. */
    class NDArray {
        /** Wraps data without copying when it is a typed array (dtype from its class; Uint16Array needs dtype "bfloat16",
         *  Uint8Array is uint8 unless dtype "bool") or an ArrayBuffer (dtype from opts, default float64, over the whole buffer);
         *  copies a number (0-d) or a nested array of numbers (rectangular; ragged is a TypeError) into a fresh float64 (or
         *  opts.dtype) array. shape defaults to the data's own shape; strides default to C-contiguous; offset defaults to 0.
         *  A view that would reach outside the buffer is a RangeError; a dtype that does not match the typed array, an unknown
         *  option or a bad argument type is a TypeError. strides/offset need a typed array or an ArrayBuffer. */
        constructor(data: NDData | ArrayBuffer | number | number[] | number[][] | number[][][], shape?: number[], opts?: { dtype?: NDType; strides?: number[]; offset?: number });
        /** New C-contiguous array of zeros (shape may be a number for 1-D); dtype defaults to float64. */
        static zeros(shape: number | number[], dtype?: NDType): NDArray;
        /** New C-contiguous array of ones. */
        static ones(shape: number | number[], dtype?: NDType): NDArray;
        /** New C-contiguous array filled with value (converted to dtype). */
        static full(shape: number | number[], value: number, dtype?: NDType): NDArray;
        /** n x m (m defaults to n) identity-like matrix: ones on the main diagonal. */
        static eye(n: number, m?: number | null, dtype?: NDType): NDArray;
        /** numpy.arange: arange(stop) or arange(start, stop, step?); length ceil((stop - start) / step), element i is
         *  start + i * step. Non-finite arguments or step 0 are a RangeError. */
        static arange(start: number, stop?: number | null, step?: number | null, dtype?: NDType): NDArray;
        /** Always copies: an NDArray or typed array into a new C-contiguous array (keeping its dtype unless one is given), a
         *  number or nested array as the constructor does. */
        static from(data: NDArray | NDData | number | number[] | number[][] | number[][][], dtype?: NDType): NDArray;
        readonly dtype: NDType;
        readonly ndim: number;
        /** Number of elements (the product of shape; 1 for 0-d). */
        readonly size: number;
        /** A fresh copy of the shape. */
        readonly shape: number[];
        /** A fresh copy of the strides, in elements. */
        readonly strides: number[];
        /** Element offset of index (0, ..., 0) in data. */
        readonly offset: number;
        /** The whole backing typed array (shared, not a copy; the same object on every read). */
        readonly data: NDData;
        /** True when the view is C-contiguous (axes of size 1 ignored), so data.subarray(offset, offset + size) is the array. */
        readonly isContiguous: boolean;
        /** transpose() with all axes reversed (a view). */
        readonly T: NDArray;
        /** The element at the given indices (exactly ndim integers, negative from the end); bool reads as a boolean. Wrong count
         *  or an index out of range is a RangeError. */
        get(...index: number[]): number;
        /** set(i, j, ..., value) writes one element (converted to dtype); returns this. */
        set(...indexThenValue: number[]): this;
        /** A view selecting along each axis (missing trailing specs keep the axis). */
        slice(...spec: NDSlice[]): NDArray;
        /** A view with the axes permuted (all axes, each once, as arguments or one array); no arguments reverses them. */
        transpose(...axes: number[]): NDArray;
        transpose(axes: number[]): NDArray;
        /** numpy.reshape in C order: a view when the strides allow it (numpy's no-copy rule), else a contiguous copy; one
         *  dimension may be -1 (inferred). A size mismatch is a RangeError. */
        reshape(...shape: number[]): NDArray;
        reshape(shape: number[]): NDArray;
        /** A view broadcast to shape by numpy rules (new leading axes and size-1 axes get stride 0); a write through it lands on
         *  the one shared element. Incompatible shapes are a RangeError. */
        broadcastTo(shape: number[]): NDArray;
        /** A view with a new axis of size 1 inserted at axis (negative counts from the end of the result). */
        expandDims(axis: number): NDArray;
        /** A view without the size-1 axes (or only the given one, which must have size 1). */
        squeeze(axis?: number): NDArray;
        /** this when already C-contiguous, else a contiguous copy. */
        contiguous(): NDArray;
        /** A C-contiguous copy with the same dtype. */
        copy(): NDArray;
        /** A C-contiguous copy converted to dtype (see NDType for the conversion rules). */
        astype(dtype: NDType): NDArray;
        /** Writes value into every element of the view; returns this. */
        fill(value: number): this;
        /** Copies src (broadcast to this shape; converted to this dtype) into the view; overlapping source and destination
         *  views are safe (the source is read completely before writing). Returns this. */
        assign(src: NDArray | NDData | number | number[] | number[][]): this;
        /** Nested JS arrays in C order (a number for 0-d; booleans for bool). */
        toArray(): number | boolean | any[];
        /** A contiguous copy as a new typed array of the dtype's class. */
        toTypedArray(): NDData;
        /** "NDArray(float64, [2, 3])". */
        toString(): string;
    }
    /** Dense and sparse linear algebra over NDArray (numpy.linalg / scipy.linalg class). NDArray is the same class object as
     *  the top-level export. The namespace's functions are created on first access; importing dyna:mathx builds only the
     *  NDArray class. */
    namespace linalg {
        const NDArray: NDArrayClass;
    }
    /** The NDArray constructor type, so `linalg.NDArray` can name the same class. */
    type NDArrayClass = typeof NDArray;

    /** Window names accepted by signal.getWindow, firwin and welch. */
    type SignalWindow = "boxcar" | "hann" | "hamming" | "blackman" | "bartlett" | "kaiser";

    /** Compiles an arithmetic string to an RPN program; no eval, no scope. */
    class Expression {
        /** Compiles an arithmetic string to an RPN program over doubles with shunting-yard; there is no eval and no scope. */
        constructor(text: string);
        /** Free variables in first-use order. */
        variables(): string[];
        /** Evaluates, reading only own data properties. */
        eval(vars?: Record<string, number>): number;
    }
}
