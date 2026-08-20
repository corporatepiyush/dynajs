/* test_mathx.js — dyna:mathx (Math additions + int/BigInt helpers).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_mathx.js
 * Prints "test_mathx: all tests passed" on success; throws on failure. */

import {
    E, Pi, Phi, Sqrt2, SqrtE, SqrtPi, Ln2, Log2E, Ln10, Log10E,
    MaxInt32, MinInt32, MaxInt64, MaxSafeInteger,
    gamma, lgamma, erf, erfc, cbrt, hypot, copysign, nextafter,
    expm1, log1p, log2, logb, scalbn, ilogb, modf, frexp, ldexp,
    remainder, fmod, isInf, isNaN as mxIsNaN, signbit, trunc, round, roundToEven,
    gcd, lcm, factorial, isPrime, abs, bitLen, popcount, besselj, besselk, besselkScaled,
} from "dyna:mathx";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    if (actual !== expected)
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected " + expected + ")");
}
function assertClose(actual, expected, tol, msg) {
    n++;
    if (!(Math.abs(actual - expected) <= tol))
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected ~" + expected + ", tol " + tol + ")");
}
function assertNaN(actual, msg) {
    n++;
    if (!Number.isNaN(actual))
        throw new Error("assertion failed: " + msg + " (got " + actual + ", expected NaN)");
}
function assertNegZero(actual, msg) {
    n++;
    if (!Object.is(actual, -0))
        throw new Error("assertion failed: " + msg + " (got " + actual + ", expected -0)");
}
function assertPosZero(actual, msg) {
    n++;
    if (!Object.is(actual, 0))
        throw new Error("assertion failed: " + msg + " (got " + actual + ", expected +0)");
}

/* ================================================================ *
 *  Constants: known values + cross-check against Math/Number where
 *  they overlap.
 * ================================================================ */
{
    assertClose(E, 2.718281828459045, 1e-15, "E");
    assertClose(Pi, 3.141592653589793, 1e-15, "Pi");
    assertClose(Phi, 1.618033988749895, 1e-15, "Phi");
    assertClose(Sqrt2, 1.4142135623730951, 1e-15, "Sqrt2");
    assertClose(SqrtE, 1.6487212707001282, 1e-15, "SqrtE");
    assertClose(SqrtPi, 1.7724538509055159, 1e-15, "SqrtPi");
    assertClose(Ln2, 0.6931471805599453, 1e-16, "Ln2");
    assertClose(Ln10, 2.302585092994046, 1e-15, "Ln10");
    assertClose(Log2E, 1 / Ln2, 1e-15, "Log2E ~ 1/Ln2");
    assertClose(Log10E, 1 / Ln10, 1e-15, "Log10E ~ 1/Ln10");

    /* Cross-check against Math: both round the SAME mathematical constant to
     * the nearest double, so they must be bit-identical, not just close. */
    assertEq(Pi, Math.PI, "Pi === Math.PI");
    assertEq(E, Math.E, "E === Math.E");
    assertEq(Sqrt2, Math.SQRT2, "Sqrt2 === Math.SQRT2");
    assertEq(Ln2, Math.LN2, "Ln2 === Math.LN2");
    assertEq(Ln10, Math.LN10, "Ln10 === Math.LN10");
    assertEq(Log2E, Math.LOG2E, "Log2E === Math.LOG2E");
    assertEq(Log10E, Math.LOG10E, "Log10E === Math.LOG10E");

    assertEq(MaxInt32, 2147483647, "MaxInt32");
    assertEq(MinInt32, -2147483648, "MinInt32");
    assertEq(MaxSafeInteger, Number.MAX_SAFE_INTEGER, "MaxSafeInteger === Number.MAX_SAFE_INTEGER");
    assertEq(MaxInt64, 9223372036854775807n, "MaxInt64");
    assert(typeof MaxInt64 === "bigint", "MaxInt64 is a BigInt");
}

/* ================================================================ *
 *  gamma / lgamma
 * ================================================================ */
{
    assertEq(gamma(5), 24, "gamma(5) = 4! = 24");
    assertEq(gamma(1), 1, "gamma(1) = 0! = 1");
    assertEq(gamma(6), 120, "gamma(6) = 5! = 120");
    assertClose(gamma(0.5), Math.sqrt(Pi), 1e-14, "gamma(0.5) ~ sqrt(pi)");

    /* The documented special cases for Gamma. */
    assertEq(gamma(Infinity), Infinity, "gamma(+Inf) = +Inf");
    assertNaN(gamma(-Infinity), "gamma(-Inf) = NaN");
    assertEq(gamma(0), Infinity, "gamma(+0) = +Inf");
    assertEq(gamma(-0), -Infinity, "gamma(-0) = -Inf");
    assertNaN(gamma(-1), "gamma(-1) = NaN (negative integer)");
    assertNaN(gamma(-2), "gamma(-2) = NaN (negative integer)");
    assertNaN(gamma(NaN), "gamma(NaN) = NaN");

    /* lgamma(x) -> [value, sign]; ln(Gamma(5)) = ln(24). */
    {
        const [v, s] = lgamma(5);
        assertClose(v, Math.log(24), 1e-12, "lgamma(5) value = ln(24)");
        assertEq(s, 1, "lgamma(5) sign = +1 (Gamma(5) > 0)");
    }
    {
        const [v] = lgamma(Infinity);
        assertEq(v, Infinity, "lgamma(+Inf) value = +Inf");
    }
    {
        const [v] = lgamma(-3); /* lgamma(-integer) = +Inf (a pole) */
        assertEq(v, Infinity, "lgamma(-3) value = +Inf (pole)");
    }
    {
        const [v] = lgamma(NaN);
        assertNaN(v, "lgamma(NaN) value = NaN");
    }
}

/* ================================================================ *
 *  erf / erfc
 * ================================================================ */
{
    assertEq(erf(0), 0, "erf(0) = 0");
    assertEq(erf(Infinity), 1, "erf(+Inf) = 1");
    assertEq(erf(-Infinity), -1, "erf(-Inf) = -1");
    assertNaN(erf(NaN), "erf(NaN) = NaN");

    assertEq(erfc(0), 1, "erfc(0) = 1");
    assertEq(erfc(Infinity), 0, "erfc(+Inf) = 0");
    assertEq(erfc(-Infinity), 2, "erfc(-Inf) = 2");
    assertNaN(erfc(NaN), "erfc(NaN) = NaN");

    /* erf(x) + erfc(x) == 1 mathematically, for several x (fp tolerance). */
    for (const x of [0.1, 0.5, 1, 1.5, 2, 3, -0.7, -2.2])
        assertClose(erf(x) + erfc(x), 1, 1e-12, "erf+erfc==1 at x=" + x);
}

/* ================================================================ *
 *  cbrt / hypot
 * ================================================================ */
{
    /* cbrt is a libm transcendental and ES leaves its precision
       implementation-defined, so an exact-equality assertion on a non-trivial
       cube root is a bug in the TEST, not the engine: macOS libm returns
       exactly 3 for cbrt(27) while glibc/x86-64 returns 3.0000000000000004.
       Found by tools/xplat-verify.sh -- this file had never run on x86-64.
       Exactness is asserted only where the value is exactly representable and
       the result is spec-forced (zero, and the sign symmetry). */
    assertClose(cbrt(27), 3, 1e-15, "cbrt(27) ~ 3");
    assertClose(cbrt(-27), -3, 1e-15, "cbrt(-27) ~ -3");
    assertEq(cbrt(0), 0, "cbrt(0) = 0");
    assertClose(cbrt(8), 2, 1e-15, "cbrt(8) ~ 2");
    assertEq(cbrt(-27), -cbrt(27), "cbrt is odd");
    /* same libm on both sides, so this one IS exact on every platform */
    assertEq(cbrt(27), Math.cbrt(27), "cbrt matches Math.cbrt");

    assertEq(hypot(3, 4), 5, "hypot(3,4) = 5");
    assertEq(hypot(0, 0), 0, "hypot(0,0) = 0");
    assertEq(hypot(3, 4), Math.hypot(3, 4), "hypot matches Math.hypot");
    /* C99 rule: infinity trumps NaN in either position. */
    assertEq(hypot(Infinity, NaN), Infinity, "hypot(Inf,NaN) = Inf (Inf beats NaN)");
    assertEq(hypot(NaN, Infinity), Infinity, "hypot(NaN,Inf) = Inf (Inf beats NaN)");
    assertNaN(hypot(NaN, 1), "hypot(NaN, finite) = NaN");
}

/* ================================================================ *
 *  copysign / signbit / nextafter
 * ================================================================ */
{
    assertEq(copysign(3, -1), -3, "copysign(3,-1) = -3");
    assertEq(copysign(-3, 1), 3, "copysign(-3,1) = 3");
    assertEq(copysign(3, -0), -3, "copysign(3,-0) = -3 (sign of -0 is negative)");
    assertEq(copysign(3, 0), 3, "copysign(3,+0) = 3");
    assertNaN(copysign(NaN, -1), "copysign(NaN,-1) is still NaN");

    assert(signbit(-1) === true, "signbit(-1)");
    assert(signbit(1) === false, "signbit(1) === false");
    assert(signbit(-0) === true, "signbit(-0)");
    assert(signbit(0) === false, "signbit(+0) === false");
    assert(signbit(-Infinity) === true, "signbit(-Inf)");
    assert(signbit(Infinity) === false, "signbit(+Inf) === false");
    /* Sign bit of NaN reflects the actual bit pattern this engine produced
     * for unary negation, not an ECMAScript-level guarantee about NaN. */
    assert(signbit(NaN) === false, "signbit(NaN) observed false on this engine/platform");
    assert(signbit(-NaN) === true, "signbit(-NaN) observed true (sign bit flipped by unary -)");

    assert(nextafter(1, 2) > 1, "nextafter(1,2) > 1");
    assert(nextafter(1, 0) < 1, "nextafter(1,0) < 1");
    assertEq(nextafter(5, 5), 5, "nextafter(x,x) = x");
    assertEq(nextafter(0, 1), Number.MIN_VALUE, "nextafter(0,1) = smallest denormal");
    assertNaN(nextafter(NaN, 1), "nextafter(NaN,y) = NaN");
    assertNaN(nextafter(1, NaN), "nextafter(x,NaN) = NaN");
}

/* ================================================================ *
 *  expm1 / log1p / log2 / logb
 * ================================================================ */
{
    assertEq(expm1(0), 0, "expm1(0) = 0");
    assertEq(log1p(0), 0, "log1p(0) = 0");
    assertClose(expm1(1), Math.E - 1, 1e-14, "expm1(1) ~ e-1");
    assertClose(log1p(Math.E - 1), 1, 1e-14, "log1p(e-1) ~ 1");
    for (const x of [0.001, 0.1, 1, 5, -0.5])
        assertClose(expm1(x), Math.expm1(x), 1e-12, "expm1 matches Math.expm1 at " + x);
    for (const x of [0.001, 0.1, 1, 5, 100])
        assertClose(log1p(x), Math.log1p(x), 1e-12, "log1p matches Math.log1p at " + x);

    assertEq(log2(8), 3, "log2(8) = 3");
    assertEq(log2(1), 0, "log2(1) = 0");
    assertEq(log2(8), Math.log2(8), "log2 matches Math.log2");

    assertEq(logb(8), 3, "logb(8) = 3");
    assertEq(logb(0.5), -1, "logb(0.5) = -1");
    assertEq(logb(0), -Infinity, "logb(0) = -Inf");
    assertEq(logb(Infinity), Infinity, "logb(Inf) = +Inf");
    assertNaN(logb(NaN), "logb(NaN) = NaN");
}

/* ================================================================ *
 *  scalbn / ldexp / frexp roundtrips
 * ================================================================ */
{
    assertEq(scalbn(1, 10), 1024, "scalbn(1,10) = 1024");
    assertEq(ldexp(1, 10), 1024, "ldexp(1,10) = 1024");
    assertEq(scalbn(3, -2), 0.75, "scalbn(3,-2) = 0.75");
    assertEq(ldexp(1, 10), scalbn(1, 10), "ldexp and scalbn agree (FLT_RADIX==2)");

    for (const x of [8, 1, 0.5, 3.14159, -12345.6789, 1e300, 1e-300]) {
        const [frac, exp] = frexp(x);
        assertClose(ldexp(frac, exp), x, Math.abs(x) * 1e-15 || 1e-300,
            "ldexp(frexp(x)) roundtrips for x=" + x);
        if (x !== 0)
            assert(Math.abs(frac) >= 0.5 && Math.abs(frac) < 1,
                "frexp fraction in [0.5,1) for x=" + x);
    }
    {
        const [frac, exp] = frexp(8);
        assertEq(frac, 0.5, "frexp(8) frac = 0.5");
        assertEq(exp, 4, "frexp(8) exp = 4");
    }
    {
        const [frac, exp] = frexp(0);
        assertPosZero(frac, "frexp(+0) frac = +0");
        assertEq(exp, 0, "frexp(+0) exp = 0");
    }
    {
        const [frac, exp] = frexp(-0);
        assertNegZero(frac, "frexp(-0) frac = -0");
        assertEq(exp, 0, "frexp(-0) exp = 0");
    }
    {
        const [frac, exp] = frexp(Infinity);
        assertEq(frac, Infinity, "frexp(+Inf) frac = +Inf");
        assertEq(exp, 0, "frexp(+Inf) exp = 0");
    }
    {
        const [frac] = frexp(NaN);
        assertNaN(frac, "frexp(NaN) frac = NaN");
    }
}

/* ================================================================ *
 *  ilogb
 * ================================================================ */
{
    assertEq(ilogb(8), 3, "ilogb(8) = 3");
    assertEq(ilogb(1), 0, "ilogb(1) = 0");
    assertEq(ilogb(0.5), -1, "ilogb(0.5) = -1");
    assertEq(ilogb(0), MinInt32, "ilogb(0) = MinInt32");
    assertEq(ilogb(Infinity), MaxInt32, "ilogb(+Inf) = MaxInt32");
    assertEq(ilogb(-Infinity), MaxInt32, "ilogb(-Inf) = MaxInt32");
    assertEq(ilogb(NaN), MaxInt32, "ilogb(NaN) = MaxInt32, NOT the raw libc sentinel");
}

/* ================================================================ *
 *  modf: [intPart, fracPart] -- note this order, and the module's own
 *  Inf special case (fracPart is NaN, not 0 -- see module header).
 * ================================================================ */
{
    {
        const [ip, fp] = modf(3.75);
        assertEq(ip, 3, "modf(3.75) intPart");
        assertClose(fp, 0.75, 1e-15, "modf(3.75) fracPart");
    }
    {
        const [ip, fp] = modf(-3.75);
        assertEq(ip, -3, "modf(-3.75) intPart");
        assertClose(fp, -0.75, 1e-15, "modf(-3.75) fracPart");
    }
    {
        const [ip, fp] = modf(5);
        assertEq(ip, 5, "modf(5) intPart");
        assertPosZero(fp, "modf(5) fracPart = +0");
    }
    {
        const [ip, fp] = modf(Infinity);
        assertEq(ip, Infinity, "modf(+Inf) intPart = +Inf");
        assertNaN(fp, "modf(+Inf) fracPart = NaN, not C99's +-0");
    }
    {
        const [ip, fp] = modf(-Infinity);
        assertEq(ip, -Infinity, "modf(-Inf) intPart = -Inf");
        assertNaN(fp, "modf(-Inf) fracPart = NaN");
    }
    {
        const [ip, fp] = modf(NaN);
        assertNaN(ip, "modf(NaN) intPart = NaN");
        assertNaN(fp, "modf(NaN) fracPart = NaN");
    }
}

/* ================================================================ *
 *  remainder / fmod
 * ================================================================ */
{
    assertClose(remainder(5.3, 2), -0.7, 1e-12, "remainder(5.3,2) rounds quotient to nearest");
    assertClose(fmod(5.3, 2), 1.3, 1e-12, "fmod(5.3,2) truncates quotient");
    assertEq(fmod(5, 2), 1, "fmod(5,2) = 1 (matches JS %)");
    assertEq(fmod(5, 2), 5 % 2, "fmod matches JS remainder operator for positive operands");

    assertEq(remainder(5.3, Infinity), 5.3, "remainder(x,+-Inf) = x");
    assertEq(fmod(5.3, Infinity), 5.3, "fmod(x,+-Inf) = x");
    assertNaN(remainder(Infinity, 2), "remainder(+-Inf,y) = NaN");
    assertNaN(fmod(Infinity, 2), "fmod(+-Inf,y) = NaN");
    assertNaN(remainder(5.3, 0), "remainder(x,0) = NaN");
    assertNaN(fmod(5.3, 0), "fmod(x,0) = NaN");
}

/* ================================================================ *
 *  isInf / isNaN / signbit (classification)
 * ================================================================ */
{
    assertEq(isInf(Infinity), true, "isInf(+Inf)");
    assertEq(isInf(-Infinity), true, "isInf(-Inf)");
    assertEq(isInf(5), false, "isInf(5) = false");
    assertEq(isInf(NaN), false, "isInf(NaN) = false");
    assertEq(isInf(Infinity, 1), true, "isInf(+Inf, sign=1) = true");
    assertEq(isInf(Infinity, -1), false, "isInf(+Inf, sign=-1) = false");
    assertEq(isInf(-Infinity, 1), false, "isInf(-Inf, sign=1) = false");
    assertEq(isInf(-Infinity, -1), true, "isInf(-Inf, sign=-1) = true");
    assertEq(isInf(-Infinity, 0), true, "isInf(-Inf, sign=0) = true (either)");

    assertEq(mxIsNaN(NaN), true, "isNaN(NaN)");
    assertEq(mxIsNaN(5), false, "isNaN(5) = false");
    assertEq(mxIsNaN(Infinity), false, "isNaN(Infinity) = false");
    /* cross-check against the global/Number versions */
    assertEq(mxIsNaN(NaN), Number.isNaN(NaN), "isNaN matches Number.isNaN(NaN)");
    assertEq(mxIsNaN(5), Number.isNaN(5), "isNaN matches Number.isNaN(5)");
}

/* ================================================================ *
 *  trunc / round / roundToEven -- the task's headline divergence:
 *  round is round-half-AWAY-from-zero; Math.round rounds a negative
 *  half toward +Infinity. roundToEven is banker's rounding.
 * ================================================================ */
{
    assertEq(trunc(2.7), 2, "trunc(2.7) = 2");
    assertEq(trunc(-2.7), -2, "trunc(-2.7) = -2");
    assertEq(trunc(2.7), Math.trunc(2.7), "trunc matches Math.trunc (positive)");
    assertEq(trunc(-2.7), Math.trunc(-2.7), "trunc matches Math.trunc (negative)");

    assertEq(round(2.5), 3, "round(2.5) = 3 (away from zero)");
    assertEq(round(-2.5), -3, "round(-2.5) = -3 (away from zero, NOT Math.round's -2)");
    assertEq(round(2.4), 2, "round(2.4) = 2");
    assertEq(round(-2.4), -2, "round(-2.4) = -2");
    assertEq(round(0.5), 1, "round(0.5) = 1");
    assertEq(round(-0.5), -1, "round(-0.5) = -1");

    /* The headline divergence from native Math.round: */
    assertEq(Math.round(2.5), 3, "sanity: Math.round(2.5) = 3 (agrees with round() here)");
    assertEq(Math.round(-2.5), -2, "sanity: Math.round(-2.5) = -2 (toward +Inf, DIFFERS from round())");
    assert(round(-2.5) !== Math.round(-2.5),
        "round() and Math.round() genuinely disagree at a negative .5 tie");

    assertEq(roundToEven(2.5), 2, "roundToEven(2.5) = 2 (nearest even)");
    assertEq(roundToEven(-2.5), -2, "roundToEven(-2.5) = -2 (nearest even)");
    assertEq(roundToEven(1.5), 2, "roundToEven(1.5) = 2");
    assertEq(roundToEven(-1.5), -2, "roundToEven(-1.5) = -2");
    assertEq(roundToEven(0.5), 0, "roundToEven(0.5) = 0");
    assertPosZero(roundToEven(0.5), "roundToEven(0.5) is +0");
    assertNegZero(roundToEven(-0.5), "roundToEven(-0.5) is -0 (sign of x preserved)");
    assertEq(roundToEven(3), 3, "roundToEven(3) = 3 (already integral)");
    assertEq(roundToEven(Infinity), Infinity, "roundToEven(+Inf) = +Inf");
    assertNaN(roundToEven(NaN), "roundToEven(NaN) = NaN");

    /* round() vs roundToEven(): the task's required headline vectors. */
    assertEq(round(2.5), 3, "round(2.5)->3");
    assertEq(roundToEven(2.5), 2, "roundToEven(2.5)->2");
    assertEq(round(-2.5), -3, "round(-2.5)->-3");
    assertEq(roundToEven(-2.5), -2, "roundToEven(-2.5)->-2");
}

/* ================================================================ *
 *  Integer/BigInt: gcd / lcm
 * ================================================================ */
{
    assertEq(gcd(12, 18), 6n, "gcd(12,18) = 6n");
    assert(typeof gcd(12, 18) === "bigint", "gcd returns a BigInt");
    assertEq(gcd(0, 0), 0n, "gcd(0,0) = 0n");
    assertEq(gcd(7, 0), 7n, "gcd(7,0) = 7n");
    assertEq(gcd(0, 7), 7n, "gcd(0,7) = 7n");
    assertEq(gcd(-12, 18), 6n, "gcd(-12,18) = 6n (absolute value)");
    assertEq(gcd(-12, -18), 6n, "gcd(-12,-18) = 6n");
    assertEq(gcd(17, 5), 1n, "gcd(17,5) = 1n (coprime)");
    assertEq(gcd(12n, 18n), 6n, "gcd accepts BigInt args too");

    assertEq(lcm(4, 6), 12n, "lcm(4,6) = 12n");
    assert(typeof lcm(4, 6) === "bigint", "lcm returns a BigInt");
    assertEq(lcm(0, 5), 0n, "lcm(0,5) = 0n");
    assertEq(lcm(5, 0), 0n, "lcm(5,0) = 0n");
    assertEq(lcm(21, 6), 42n, "lcm(21,6) = 42n");
    assertEq(lcm(-4, 6), 12n, "lcm(-4,6) = 12n (absolute value)");

    /* gcd/lcm identity: a*b == gcd(a,b)*lcm(a,b) for positive a,b, cross-
     * checked against a pure-JS BigInt reference over several pairs. */
    function gcdRef(a, b) { a = a < 0n ? -a : a; b = b < 0n ? -b : b; while (b) { [a, b] = [b, a % b]; } return a; }
    for (const [a, b] of [[12, 18], [7, 13], [100, 75], [1, 1], [270, 192], [999999937, 2]]) {
        const g = gcdRef(BigInt(a), BigInt(b));
        assertEq(gcd(a, b), g, "gcd matches reference for (" + a + "," + b + ")");
        const want = g === 0n ? 0n : (BigInt(a) * BigInt(b)) / g;
        const l = want < 0n ? -want : want;
        assertEq(lcm(a, b), l, "lcm matches reference for (" + a + "," + b + ")");
    }

    /* lcm exceeding 64 bits: 2^40 and 2^40+1 are coprime (consecutive
     * integers), so lcm = their exact product, ~2^80 -- well past
     * UINT64_MAX -- cross-checked against pure-JS BigInt arithmetic. */
    {
        const a = 2n ** 40n, b = 2n ** 40n + 1n;
        const expected = a * b; /* gcd(a,b) = 1 since consecutive integers */
        const got = lcm(a, b);
        assertEq(got, expected, "lcm exceeding 64 bits matches pure-JS BigInt reference");
        assert(got > 0xFFFFFFFFFFFFFFFFn, "lcm result genuinely exceeds 2^64-1");
    }
    {
        /* Another >64-bit case with non-trivial gcd, still cross-checked. */
        const a = 6n * 2n ** 40n, b = 10n * 2n ** 40n;
        const g = gcdRef(a, b);
        const expected = (a * b) / g;
        assertEq(lcm(a, b), expected, "lcm with non-trivial gcd, exceeding 64 bits");
    }
}

/* ================================================================ *
 *  Integer/BigInt: factorial
 * ================================================================ */
{
    assertEq(factorial(0), 1n, "factorial(0) = 1n");
    assertEq(factorial(1), 1n, "factorial(1) = 1n");
    assertEq(factorial(5), 120n, "factorial(5) = 120n");
    assertEq(factorial(10), 3628800n, "factorial(10) = 3628800n");
    assert(typeof factorial(5) === "bigint", "factorial returns a BigInt");

    /* The task's required known vector: factorial(20), still within 64
     * bits (the fast uint64 path, no bignum/eval tail needed). */
    assertEq(factorial(20), 2432902008176640000n, "factorial(20) known value");

    /* factorial(21) is the first value that overflows 64 bits (both signed
     * and unsigned), forcing the bignum-accumulator + BigInt-literal path;
     * cross-check a wide range against a pure-JS BigInt reference
     * implementation, so this differentially proves the bignum tail. */
    function factorialRef(n) {
        let r = 1n;
        for (let i = 2n; i <= BigInt(n); i++) r *= i;
        return r;
    }
    for (const k of [2, 6, 19, 20, 21, 22, 25, 30, 40, 50, 64, 99, 100, 170, 250, 500, 999])
        assertEq(factorial(k), factorialRef(k), "factorial(" + k + ") matches pure-JS BigInt reference");

    /* Negative / non-integer / too-large all throw, never silently wrap. */
    let threw;
    threw = false; try { factorial(-1); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(-1) throws RangeError");
    threw = false; try { factorial(3.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(3.5) throws RangeError (non-integer)");
    threw = false; try { factorial(100000000); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(1e8) throws RangeError (over the compute cap)");
}

/* ================================================================ *
 *  Integer/BigInt: isPrime
 * ================================================================ */
{
    assertEq(isPrime(0), false, "isPrime(0) = false");
    assertEq(isPrime(1), false, "isPrime(1) = false");
    assertEq(isPrime(2), true, "isPrime(2) = true (smallest prime)");
    assertEq(isPrime(3), true, "isPrime(3) = true");
    assertEq(isPrime(4), false, "isPrime(4) = false");
    assertEq(isPrime(-7), false, "isPrime(-7) = false (negative never prime)");
    assertEq(isPrime(9), false, "isPrime(9) = false (3^2)");
    assertEq(isPrime(17), true, "isPrime(17) = true");
    assertEq(isPrime(97), true, "isPrime(97) = true");
    assertEq(isPrime(100), false, "isPrime(100) = false");

    /* Famous large primes/composites used constantly in practice. */
    assertEq(isPrime(1000000007n), true, "isPrime(1e9+7) = true (the classic mod-prime)");
    assertEq(isPrime(1000000009n), true, "isPrime(1e9+9) = true (the other classic mod-prime)");
    assertEq(isPrime(1000000006n), false, "isPrime(1e9+6) = false (even)");

    /* A genuinely large 64-bit-range prime forcing real Miller-Rabin work
     * (well past the small-prime trial-division cutoff): the Mersenne
     * prime 2^61-1. */
    assertEq(isPrime(2305843009213693951n), true, "isPrime(2^61-1) = true (Mersenne prime M61)");
    /* 2^63-1 is composite: 63 = 7*9 is composite, so 2^7-1=127 divides it
     * (2^d-1 | 2^n-1 whenever d|n) -- also exercises the very top of the
     * signed 64-bit domain. */
    assertEq(isPrime(9223372036854775807n), false, "isPrime(2^63-1) = false (composite: divisible by 127)");
    assertEq(9223372036854775807n % 127n, 0n, "sanity: 127 divides 2^63-1");

    /* Numbers and BigInts agree for values representable exactly as both. */
    assertEq(isPrime(97), isPrime(97n), "isPrime agrees for Number vs BigInt input");
}

/* ================================================================ *
 *  Integer/BigInt: abs / bitLen / popcount (strict BigInt-only)
 * ================================================================ */
{
    assertEq(abs(5n), 5n, "abs(5n) = 5n");
    assertEq(abs(-5n), 5n, "abs(-5n) = 5n");
    assertEq(abs(0n), 0n, "abs(0n) = 0n");
    assertEq(abs(-9223372036854775808n), 9223372036854775808n,
        "abs(INT64_MIN) = 2^63 (the one magnitude with no positive int64 twin)");

    let threw = false;
    try { abs(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "abs(Number) throws TypeError -- BigInt required");

    assertEq(bitLen(0n), 0, "bitLen(0n) = 0");
    assertEq(bitLen(1n), 1, "bitLen(1n) = 1");
    assertEq(bitLen(255n), 8, "bitLen(255n) = 8 (0b11111111)");
    assertEq(bitLen(256n), 9, "bitLen(256n) = 9 (0b100000000)");
    assertEq(bitLen(-255n), 8, "bitLen(-255n) = 8 (operates on magnitude)");
    assertEq(bitLen(-9223372036854775808n), 64, "bitLen(INT64_MIN) = 64 (|INT64_MIN| = 2^63)");

    assertEq(popcount(0n), 0, "popcount(0n) = 0");
    assertEq(popcount(255n), 8, "popcount(255n) = 8 (all bits set)");
    assertEq(popcount(256n), 1, "popcount(256n) = 1 (single bit)");
    assertEq(popcount(-255n), 8, "popcount(-255n) = 8 (operates on magnitude)");
    assertEq(popcount(-9223372036854775808n), 1, "popcount(INT64_MIN) = 1 (|INT64_MIN| = 2^63, one bit)");

    threw = false;
    try { bitLen(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "bitLen(Number) throws TypeError -- BigInt required");
    threw = false;
    try { popcount(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "popcount(Number) throws TypeError -- BigInt required");
}

/* besselj: libm's jn is an O(n) recurrence, so jn(INT_MAX, 3) ran 11646 ms for
   an answer of 0. The (|x|/2)^n / n! bound skips it. Both halves are pinned:
   that it is FAST, and that it did not change any value it used to return. */
{
    const t0 = Date.now();
    assertEq(besselj(2147483647, 3), 0, "a huge order underflows to 0");
    assertEq(besselj(-2147483648, 1), 0, "and so does INT_MIN, whose negation is itself");
    assert(Date.now() - t0 < 1000,
           "a huge order returns without running the recurrence (took " +
           (Date.now() - t0) + " ms)");

    /* the bound must NOT fire where a real value exists */
    assertEq(besselj(50, 10), 1.784513607871594e-30, "an order libm can still reach is unchanged");
    assert(besselj(200, 10) !== 0, "6.9e-236 is representable, so it is not zeroed");
    assert(besselj(200, 100) !== 0, "nor is 2.1e-41");

    /* independent oracle: the recurrence J_{n-1} + J_{n+1} = (2n/x) J_n is an
       identity the implementation does not compute */
    let worst = 0;
    for (const x of [0.5, 1, 2.5, 3, 7, 10, 25, 100])
        for (let k = 1; k <= 120; k++) {
            const lhs = besselj(k - 1, x) + besselj(k + 1, x);
            const rhs = (2 * k / x) * besselj(k, x);
            worst = Math.max(worst, Math.abs(lhs - rhs) / Math.max(1e-300, Math.abs(rhs)));
        }
    assert(worst < 1e-12, "960 orders satisfy the Bessel recurrence (worst " + worst + ")");

    /* J_-n = (-1)^n J_n must survive the fast path in both parities */
    assertEq(besselj(-3, 2.5), -besselj(3, 2.5), "odd negative order flips sign");
    assertEq(besselj(-4, 2.5), besselj(4, 2.5), "even negative order does not");
    /* When |x| is comparable to n the bound cannot fire and the recurrence runs
       to completion -- 2668 ms at n = x = INT_MAX. That is REFUSED, while the
       same order with a small x is still answered instantly. */
    let refused = false;
    try { besselj(2147483647, 2147483647); } catch (e) { refused = e instanceof RangeError; }
    assert(refused, "an order that would recurse for seconds is refused");
    assertEq(besselj(2147483647, 3), 0, "the same order with a small x still answers");
    assert(besselj(16777216, 1e300) !== undefined, "the cap itself is allowed");

    assertEq(besselj(5, 0), 0, "J_n(0) is 0 for n >= 1");
    assertEq(besselj(0, 0), 1, "and J_0(0) is 1");
}

/* besselk: the NaN->int UBSan and large-order guard (dyn_besselk_scaled).
 * K_nu(x) -> 0 as |nu|->inf or x->Inf, and NaN propagates. The (int)floor
 * in mx_besselk_small is UB on NaN, so the core now guards it. */
{
    // besselk may be missing in some minimal builds; guard
    if (typeof besselk === "function") {
        assertNaN(besselk(NaN, 1), "besselk(NaN,1) NaN");
        assertNaN(besselk(1, NaN), "besselk(1,NaN) NaN");
        assertNaN(besselk(NaN, NaN), "besselk(NaN,NaN) NaN");
        assertNaN(besselkScaled(NaN, 1), "besselkScaled(NaN,1) NaN");
        assertNaN(besselkScaled(1, NaN), "besselkScaled(1,NaN) NaN");
        assertEq(besselk(Infinity, 1), 0, "besselk(Inf,1) 0");
        assertEq(besselk(-Infinity, 1), 0, "besselk(-Inf,1) 0");
        assertEq(besselk(1, Infinity), 0, "besselk(1,Inf) 0");
        assertEq(besselkScaled(Infinity, 1), 0, "besselkScaled(Inf,1) 0");
        assertEq(besselkScaled(1, Infinity), 0, "besselkScaled(1,Inf) 0");

        assertEq(besselk(0, 0), Infinity, "besselk(0,0) Inf");
        assertNaN(besselk(0, -1), "besselk(0,-1) NaN");
        for (const [nu, x] of [[0,1],[0.5,1],[1,1],[2,0.5],[5,2]]) {
            const k = besselk(nu, x);
            const ks = besselkScaled(nu, x);
            assert(Number.isFinite(k), `besselk(${nu},${x}) finite`);
            assert(Number.isFinite(ks), `besselkScaled(${nu},${x}) finite`);
            const expect = k * Math.exp(x);
            const rel = Math.abs(ks - expect) / Math.max(1e-300, Math.abs(expect));
            assert(rel < 1e-12, `besselkScaled vs k*exp at ${nu},${x} rel ${rel}`);
        }
        for (const [nu,x] of [[NaN,1],[1,NaN],[Infinity,1],[1,Infinity],[1e9,1]]) {
            let threw=false; try{besselk(nu,x);}catch(e){threw=true;}
            assert(!threw, `besselk(${nu},${x}) should not throw`);
        }
    }
}

print("test_mathx: all tests passed (" + n + " assertions)");
