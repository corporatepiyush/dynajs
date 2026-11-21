// covers: dyna:mathx.Ln2
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_ln2");
import { cpuNow } from "../lib/cpuclock.js";
import {
    E, Pi, Phi, Sqrt2, SqrtE, SqrtPi, Ln2, Log2E, Ln10, Log10E,
    MaxInt32, MinInt32, MaxInt64, MaxSafeInteger,
    gamma, lgamma, beta, betaln, gammaln, erf, erfc, cbrt, hypot, copysign, nextafter,
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

__kitS.section("Log2E ~ 1/Ln2", () => {
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
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
