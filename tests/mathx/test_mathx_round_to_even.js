// covers: dyna:mathx.roundToEven
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_round_to_even");
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

__kitS.section("trunc(2.7) = 2", () => {
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

    assertEq(round(2.5), 3, "round(2.5)->3");
    assertEq(roundToEven(2.5), 2, "roundToEven(2.5)->2");
    assertEq(round(-2.5), -3, "round(-2.5)->-3");
    assertEq(roundToEven(-2.5), -2, "roundToEven(-2.5)->-2");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
