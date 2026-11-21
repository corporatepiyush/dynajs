// covers: dyna:mathx.ilogb
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_ilogb");
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

__kitS.section("ilogb(8) = 3", () => {
    assertEq(ilogb(8), 3, "ilogb(8) = 3");
    assertEq(ilogb(1), 0, "ilogb(1) = 0");
    assertEq(ilogb(0.5), -1, "ilogb(0.5) = -1");
    assertEq(ilogb(0), MinInt32, "ilogb(0) = MinInt32");
    assertEq(ilogb(Infinity), MaxInt32, "ilogb(+Inf) = MaxInt32");
    assertEq(ilogb(-Infinity), MaxInt32, "ilogb(-Inf) = MaxInt32");
    assertEq(ilogb(NaN), MaxInt32, "ilogb(NaN) = MaxInt32, NOT the raw libc sentinel");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
