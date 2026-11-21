// covers: dyna:mathx.isInf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_is_inf");
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

__kitS.section("isInf(5) = false", () => {
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
    assertEq(mxIsNaN(NaN), Number.isNaN(NaN), "isNaN matches Number.isNaN(NaN)");
    assertEq(mxIsNaN(5), Number.isNaN(5), "isNaN matches Number.isNaN(5)");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
