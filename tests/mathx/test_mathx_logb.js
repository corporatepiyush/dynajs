// covers: dyna:mathx.logb
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_logb");
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

__kitS.section("expm1(0) = 0", () => {
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
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
