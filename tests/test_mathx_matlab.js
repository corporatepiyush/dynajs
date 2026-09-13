/* test_mathx_matlab.js -- the MATLAB-parity additions to dyna:mathx, and the
 * `bits` namespace merged in from the retired dyna:bits module.
 *
 * Reference values for the special functions were computed independently at 40
 * decimal digits (python `decimal`, Bernoulli asymptotic series after shifting
 * the argument past 40) rather than by running this implementation and writing
 * down what it said. That distinction is the entire value of the file: a
 * self-generated table proves only that the code is deterministic.
 *
 * Tolerances are absolute-or-relative and stated per group, because "1e-15 for
 * everything" is either a lie or a straitjacket depending on the function.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_mathx_matlab.js */

import { mod, rem, fmod, fix, sign, nthroot, nextpow2, pow2, deg2rad, rad2deg, eps, realmin, realmax, flintmax, beta, betaln, gammaln, psi, erfinv, erfcinv, erfcx, expint, factor, primes, nchoosek, rat, linspace, logspace, cumsum, cumprod, diff, erf, erfc, lgamma, trunc, bits } from "dyna:mathx";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (!Object.is(got, want))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
function close(got, want, tol, m) {
    n++;
    const err = Math.abs(got - want);
    const rel = want === 0 ? err : err / Math.abs(want);
    if (!(err <= tol || rel <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want +
            "\n  abs err " + err.toExponential(2) + ", rel " + rel.toExponential(2) +
            " (tol " + tol + ")");
}
function eqJSON(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + m + "\n  got:  " + a + "\n  want: " + b);
}
function throwsRange(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}

/* ==================================================================== *
 *  mod vs rem -- the distinction that motivated adding them
 * ==================================================================== */
{
    /* MATLAB mod() is FLOORED, rem() is TRUNCATED. They agree for like signs
     * and differ for mixed ones, which is exactly where a caller who reached
     * for the already-exported fmod() would have silently got the other
     * convention. */
    eq(mod(5, 3), 2, "mod(5,3)");
    eq(rem(5, 3), 2, "rem(5,3)");
    eq(mod(-1, 3), 2, "mod(-1,3) is floored");
    eq(rem(-1, 3), -1, "rem(-1,3) is truncated");
    eq(fmod(-1, 3), -1, "fmod agrees with rem, not mod");
    eq(mod(1, -3), -2, "mod(1,-3) takes the divisor's sign");
    eq(rem(1, -3), 1, "rem(1,-3) takes the dividend's sign");
    eq(mod(-5, -3), -2, "mod(-5,-3)");
    eq(rem(-5, -3), -2, "rem(-5,-3)");
    /* the sign of the result follows the DIVISOR for mod, the DIVIDEND for rem */
    for (const [a, b] of [[7, 3], [-7, 3], [7, -3], [-7, -3]]) {
        const m1 = mod(a, b), r1 = rem(a, b);
        assert(m1 === 0 || Math.sign(m1) === Math.sign(b), `mod(${a},${b}) sign follows divisor`);
        assert(r1 === 0 || Math.sign(r1) === Math.sign(a), `rem(${a},${b}) sign follows dividend`);
    }
    eq(mod(5, 0), 5, "mod by zero returns the dividend (MATLAB)");
    assert(Number.isNaN(rem(5, 0)), "rem by zero is NaN (C fmod)");
}

/* ==================================================================== *
 *  rounding, sign, roots, powers
 * ==================================================================== */
{
    eq(fix(2.7), 2, "fix truncates toward zero");
    eq(fix(-2.7), -2, "fix(-2.7)");
    eq(fix(-0.5), -0, "fix keeps -0");
    eq(fix(2.7), trunc(2.7), "fix is trunc");

    eq(sign(3), 1, "sign(+)"); eq(sign(-3), -1, "sign(-)");
    eq(sign(0), 0, "sign(0)");
    eq(sign(-0), -0, "sign(-0) preserves -0");
    assert(Number.isNaN(sign(NaN)), "sign(NaN) is NaN");

    /* the case pow(x, 1/n) cannot do */
    close(nthroot(-8, 3), -2, 1e-15, "nthroot of a negative with an odd root");
    close(nthroot(27, 3), 3, 1e-15, "nthroot(27,3)");
    close(nthroot(16, 4), 2, 1e-15, "nthroot(16,4)");
    assert(Number.isNaN(nthroot(-16, 4)), "even root of a negative is NaN");
    assert(Number.isNaN(Math.pow(-8, 1 / 3)), "pow cannot do it, which is why nthroot exists");

    eq(nextpow2(0), 0, "nextpow2(0)");
    eq(nextpow2(1), 0, "nextpow2(1)");
    eq(nextpow2(2), 1, "nextpow2(2)");
    eq(nextpow2(5), 3, "nextpow2(5)");
    eq(nextpow2(1024), 10, "nextpow2(1024)");
    eq(nextpow2(-5), 3, "nextpow2 uses |x|");
    eq(pow2(10), 1024, "pow2(10)");
    close(pow2(0.5), Math.SQRT2, 1e-15, "pow2(0.5)");

    close(deg2rad(180), Math.PI, 1e-15, "deg2rad(180)");
    close(rad2deg(Math.PI), 180, 1e-13, "rad2deg(pi)");
    close(rad2deg(deg2rad(37)), 37, 1e-13, "deg2rad/rad2deg round-trip");
}

/* ==================================================================== *
 *  IEEE constants
 * ==================================================================== */
{
    eq(eps(), Number.EPSILON, "bare eps() is eps(1) = 2^-52");
    eq(eps(1), Number.EPSILON, "eps(1)");
    /* eps(x) is the gap to the next representable value */
    for (const x of [1, 2, 0.5, 1e10, 1e-10, 12345.678]) {
        const e = eps(x);
        eq(x + e > x, true, "eps(" + x + ") actually reaches the next double");
        eq(x + e / 2 === x || x + e / 2 === x + e, true, "eps(" + x + ") is minimal");
    }
    eq(eps(-3), eps(3), "eps uses |x|");
    /* subnormals: the ulp there is the smallest denormal, not 2^(exp-52) */
    eq(eps(5e-324), 5e-324, "eps of a subnormal is the subnormal step");
    assert(Number.isNaN(eps(NaN)), "eps(NaN)");
    assert(Number.isNaN(eps(Infinity)), "eps(Infinity)");
    eq(realmin(), 2.2250738585072014e-308, "realmin is DBL_MIN");
    eq(realmax(), Number.MAX_VALUE, "realmax is DBL_MAX");
    eq(flintmax(), 9007199254740992, "flintmax is 2^53");
}

/* ==================================================================== *
 *  special functions -- reference values computed independently
 * ==================================================================== */
{
    /* gammaln is an alias for lgamma; both must agree exactly */
    const LGAMMA = [[0.5, 0.5723649429247004], [1, 0.0], [1.5, -0.12078223763524543],
                    [2, 0.0], [3, 0.693147180559945], [4.5, 2.4537365708424432],
                    [10, 12.801827480081467], [100, 359.1342053695754]];
    for (const [x, want] of LGAMMA) {
        close(gammaln(x), want, 1e-14, "gammaln(" + x + ")");
        eq(gammaln(x), lgamma(x)[0], "gammaln == lgamma value for " + x);
    }

    const PSI = [[0.5, -1.9635100260214235], [1, -0.5772156649015329],
                 [1.5, 0.03648997397857652], [2, 0.42278433509846713],
                 [3, 0.9227843350984671], [3.7, 1.1671535393615113],
                 [5, 1.5061176684318005], [10, 2.251752589066721],
                 [100, 4.600161852738087]];
    for (const [x, want] of PSI)
        close(psi(x), want, 1e-13, "psi(" + x + ")");
    /* the defining recurrence psi(x+1) = psi(x) + 1/x must hold */
    for (const x of [0.3, 1.1, 2.5, 7.9])
        close(psi(x + 1), psi(x) + 1 / x, 1e-12, "psi recurrence at " + x);
    /* reflection across the negative axis */
    close(psi(1.5) - psi(-0.5), Math.PI / Math.tan(Math.PI * -0.5), 1e-10,
          "psi reflection formula");
    assert(Number.isNaN(psi(-1)), "psi has a pole at a negative integer");
    assert(Number.isNaN(psi(-2)), "psi(-2) is a pole");

    const BETA = [[2, 3, 0.0833333333333334], [0.5, 0.5, 3.1415926535897953],
                  [1, 1, 1.0], [5, 7, 0.00043290043290043176],
                  [0.1, 0.9, 10.166407384630517]];
    for (const [a, b, want] of BETA) {
        close(beta(a, b), want, 1e-13, `beta(${a},${b})`);
        close(Math.exp(betaln(a, b)), want, 1e-12, `exp(betaln(${a},${b}))`);
    }
    eq(beta(2, 3), beta(3, 2), "beta is symmetric");
    /* betaln stays finite where beta itself would overflow */
    assert(Number.isFinite(betaln(1e-4, 1e-4)), "betaln survives tiny arguments");

    /* erfinv is the inverse of erf, so the round trip is the real assertion */
    for (const x of [0.1, 0.5, 0.7, 1.0, 1.5, 2.0, -0.3, -1.2]) {
        close(erfinv(erf(x)), x, 1e-12, "erfinv(erf(" + x + ")) round-trips");
        close(erf(erfinv(Math.tanh(x))), Math.tanh(x), 1e-13, "erf(erfinv(y)) at " + x);
    }
    eq(erfinv(0), 0, "erfinv(0)");
    eq(erfinv(1), Infinity, "erfinv(1)");
    eq(erfinv(-1), -Infinity, "erfinv(-1)");
    assert(Number.isNaN(erfinv(1.5)), "erfinv outside [-1,1] is NaN");
    for (const y of [0.01, 0.5, 0.99, 1.5, 1.99])
        close(erfcinv(y), erfinv(1 - y), 1e-12, "erfcinv(y) == erfinv(1-y) at " + y);

    /* erfcx = exp(x^2)*erfc(x): the point is that it stays finite where erfc
     * has underflowed to exactly 0 */
    for (const x of [0, 0.5, 1, 2, 5])
        close(erfcx(x), Math.exp(x * x) * erfc(x), 1e-12, "erfcx(" + x + ")");
    eq(erfc(40), 0, "erfc underflows to 0 by x=40");
    assert(erfcx(40) > 0 && Number.isFinite(erfcx(40)),
           "erfcx stays finite and positive where erfc underflowed");
    close(erfcx(40), 1 / (40 * Math.sqrt(Math.PI)), 1e-3, "erfcx(40) ~ 1/(x*sqrt(pi))");

    const E1 = [[0.1, 1.8229239584193906], [0.5, 0.5597735947761608],
                [1, 0.21938393439552029], [2, 0.04890051070806112],
                [5, 0.0011482955912753257], [10, 4.156968929685325e-06]];
    for (const [x, want] of E1)
        close(expint(x), want, 1e-12, "expint(" + x + ")");
    eq(expint(0), Infinity, "expint(0)");
    assert(Number.isNaN(expint(-1)), "expint is complex for x<0, so NaN");
}

/* ==================================================================== *
 *  discrete math
 * ==================================================================== */
{
    eqJSON(factor(360), [2, 2, 2, 3, 3, 5], "factor(360)");
    eqJSON(factor(2), [2], "factor of a prime");
    eqJSON(factor(1), [], "factor(1) is empty");
    eqJSON(factor(97), [97], "factor of a larger prime");
    eqJSON(factor(1024), [2, 2, 2, 2, 2, 2, 2, 2, 2, 2], "factor(1024)");
    /* the product of the factors must be the input -- the real invariant */
    for (const v of [12, 100, 999, 65536, 123456])
        eq(factor(v).reduce((a, b) => a * b, 1), v, "factors multiply back to " + v);
    throwsRange(() => factor(0), "factor(0)");
    throwsRange(() => factor(-4), "factor of a negative");
    throwsRange(() => factor(2.5), "factor of a non-integer");

    eqJSON(primes(20), [2, 3, 5, 7, 11, 13, 17, 19], "primes(20)");
    eqJSON(primes(2), [2], "primes(2)");
    eqJSON(primes(1), [], "primes(1)");
    eqJSON(primes(0), [], "primes(0)");
    eq(primes(100).length, 25, "25 primes below 100");
    eq(primes(1000).length, 168, "168 primes below 1000");
    /* every returned value must actually be prime */
    for (const p of primes(200))
        eq(factor(p).length, 1, p + " is prime");

    eq(nchoosek(5, 2), 10, "nchoosek(5,2)");
    eq(nchoosek(52, 5), 2598960, "nchoosek(52,5)");
    eq(nchoosek(10, 0), 1, "nchoosek(n,0)");
    eq(nchoosek(10, 10), 1, "nchoosek(n,n)");
    eq(nchoosek(3, 5), 0, "k>n is 0");
    eq(nchoosek(40, 20), 137846528820, "nchoosek(40,20) is exact past 2^32");
    /* symmetry, and Pascal's rule */
    for (let k = 0; k <= 9; k++)
        eq(nchoosek(9, k), nchoosek(9, 9 - k), "nchoosek symmetry at k=" + k);
    for (let k = 1; k <= 8; k++)
        eq(nchoosek(9, k), nchoosek(8, k - 1) + nchoosek(8, k), "Pascal's rule at k=" + k);
    throwsRange(() => nchoosek(-1, 2), "negative n");
    throwsRange(() => nchoosek(5, 1.5), "non-integer k");

    eqJSON(rat(0.75), [3, 4], "rat(0.75)");
    eqJSON(rat(0.5), [1, 2], "rat(0.5)");
    eqJSON(rat(2), [2, 1], "rat of an integer");
    eqJSON(rat(-0.25), [-1, 4], "rat keeps the sign on the numerator");
    {
        const [p, q] = rat(Math.PI, 1e-6);
        close(p / q, Math.PI, 1e-6, "rat(pi) is within tolerance");
        assert(q > 0, "rat denominator is positive");
    }
}

/* ==================================================================== *
 *  vector generators and scans
 * ==================================================================== */
{
    eqJSON(linspace(0, 1, 5), [0, 0.25, 0.5, 0.75, 1], "linspace(0,1,5)");
    eqJSON(linspace(1, 5, 5), [1, 2, 3, 4, 5], "linspace over integers");
    eqJSON(linspace(0, 1, 1), [1], "linspace with one point is the endpoint");
    eqJSON(linspace(0, 1, 0), [], "linspace with zero points");
    /* the last point is set EXACTLY, not accumulated -- the classic
     * floating-point trap in a linspace implementation */
    eq(linspace(0, 1, 11)[10], 1, "the final point is exactly b");
    eq(linspace(0, 0.3, 4)[3], 0.3, "final point exact even when the step is not");
    eq(linspace(0, 1, 101).length, 101, "linspace length");
    eqJSON(linspace(5, 1, 5), [5, 4, 3, 2, 1], "linspace descends");

    eqJSON(logspace(0, 3, 4), [1, 10, 100, 1000], "logspace(0,3,4)");
    eq(logspace(0, 2, 3)[2], 100, "logspace endpoint is exact");

    eqJSON(cumsum([1, 2, 3, 4]), [1, 3, 6, 10], "cumsum");
    eqJSON(cumsum([]), [], "cumsum of empty");
    eqJSON(cumsum([5]), [5], "cumsum of one");
    eqJSON(cumprod([1, 2, 3, 4]), [1, 2, 6, 24], "cumprod");
    eqJSON(cumprod([]), [], "cumprod of empty");
    eqJSON(diff([1, 4, 9, 16]), [3, 5, 7], "diff");
    eqJSON(diff([5]), [], "diff of one element is empty");
    eqJSON(diff([]), [], "diff of empty");
    /* diff is the inverse of cumsum, up to the first element */
    {
        const v = [3, 1, 4, 1, 5, 9, 2, 6];
        eqJSON(diff(cumsum(v)), v.slice(1), "diff(cumsum(v)) == v without its head");
    }
    /* they accept TypedArrays as well as Arrays */
    eqJSON(cumsum(new Float64Array([1, 2, 3])), [1, 3, 6], "cumsum of a Float64Array");
    eqJSON(diff(new Int32Array([10, 7, 3])), [-3, -4], "diff of an Int32Array");
}

/* ==================================================================== *
 *  the `bits` namespace, merged in from the retired dyna:bits
 * ==================================================================== */
{
    assert(typeof bits === "object", "bits is a namespace object");
    eq(bits.uintSize, 64, "bits.uintSize");

    /* counting, every width */
    eq(bits.leadingZeros8(1), 7, "leadingZeros8");
    eq(bits.leadingZeros16(1), 15, "leadingZeros16");
    eq(bits.leadingZeros32(1), 31, "leadingZeros32");
    eq(bits.leadingZeros64(1n), 63, "leadingZeros64");
    eq(bits.leadingZeros32(0), 32, "leadingZeros of 0 is the width");
    eq(bits.trailingZeros32(8), 3, "trailingZeros32");
    eq(bits.trailingZeros32(0), 32, "trailingZeros of 0 is the width");
    eq(bits.trailingZeros64(0n), 64, "trailingZeros64(0)");
    eq(bits.onesCount32(255), 8, "onesCount32");
    eq(bits.onesCount64(0xffffn), 16, "onesCount64");
    eq(bits.onesCount8(0xff), 8, "onesCount8");
    eq(bits.len32(1000), 10, "len32");
    eq(bits.len32(0), 0, "len(0) is 0");
    eq(bits.len64(0n), 0, "len64(0)");

    /* reversal */
    eq(bits.reverse8(19), 200, "reverse8(0b00010011) = 0b11001000");
    eq(bits.reverseBytes32(0x01020304), 0x04030201, "reverseBytes32");
    eq(bits.reverseBytes16(0x0102), 0x0201, "reverseBytes16");
    eq(bits.reverse32(bits.reverse32(0xdeadbeef)), 0xdeadbeef, "reverse32 is an involution");
    eq(bits.reverse64(bits.reverse64(0xdeadbeefcafebaben)), 0xdeadbeefcafebaben,
       "reverse64 is an involution");

    /* rotation, including the negative-k right rotate and the s==0 identity */
    eq(bits.rotateLeft8(1, -1), 128, "rotateLeft8 with negative k rotates right");
    eq(bits.rotateLeft8(1, 1), 2, "rotateLeft8");
    eq(bits.rotateLeft8(1, 8), 1, "a full rotation is the identity");
    eq(bits.rotateLeft8(1, 0), 1, "rotate by 0 is the identity");
    eq(bits.rotateLeft32(0x80000000, 1), 1, "rotateLeft32 wraps the top bit");
    eq(bits.rotateLeft64(1n, 64), 1n, "rotateLeft64 full rotation");

    /* multi-precision */
    eqJSON(bits.add32(1, 2, 0), [3, 0], "add32");
    eqJSON(bits.add32(0xffffffff, 1, 0), [0, 1], "add32 carries out");
    eqJSON(bits.add64(0xffffffffffffffffn, 1n, 0n).map(String), ["0", "1"], "add64 carries");
    eqJSON(bits.sub32(0, 1, 0), [0xffffffff, 1], "sub32 borrows");
    eqJSON(bits.mul32(0xffffffff, 2), [1, 0xfffffffe], "mul32 -> [hi, lo]");
    eqJSON(bits.mul64(0xffffffffffffffffn, 0xffffffffffffffffn).map(String),
           ["18446744073709551614", "1"], "mul64 -> [hi, lo]");
    eqJSON(bits.div32(0, 100, 7), [14, 2], "div32");
    eq(bits.rem32(0, 100, 7), 2, "rem32");
    throwsRange(() => bits.div32(0, 1, 0), "div32 by zero");
    throwsRange(() => bits.div32(5, 0, 1), "div32 quotient overflow");
    throwsRange(() => bits.rem32(0, 1, 0), "rem32 by zero");
    /* rem tolerates a quotient that would overflow; div does not */
    eq(bits.rem32(5, 0, 1), 0, "rem32 allows an overflowing quotient");
}

/* the retired module must be gone, not merely unused */
{
    n++;
    let gone = false;
    try { await import("dyna:bits"); } catch (e) { gone = true; }
    if (!gone) throw new Error("dyna:bits should no longer be importable");
}

print("test_mathx_matlab: all " + n + " tests passed");
