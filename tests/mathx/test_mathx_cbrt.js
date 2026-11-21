// covers: dyna:mathx.cbrt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_cbrt");
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

__kitS.section("cbrt(27) ~ 3", () => {
    assertClose(cbrt(27), 3, 1e-15, "cbrt(27) ~ 3");
    assertClose(cbrt(-27), -3, 1e-15, "cbrt(-27) ~ -3");
    assertEq(cbrt(0), 0, "cbrt(0) = 0");
    assertClose(cbrt(8), 2, 1e-15, "cbrt(8) ~ 2");
    assertEq(cbrt(-27), -cbrt(27), "cbrt is odd");
    assertEq(cbrt(27), Math.cbrt(27), "cbrt matches Math.cbrt");

    assertEq(hypot(3, 4), 5, "hypot(3,4) = 5");
    assertEq(hypot(0, 0), 0, "hypot(0,0) = 0");
    assertEq(hypot(3, 4), Math.hypot(3, 4), "hypot matches Math.hypot");
    assertEq(hypot(Infinity, NaN), Infinity, "hypot(Inf,NaN) = Inf (Inf beats NaN)");
    assertEq(hypot(NaN, Infinity), Infinity, "hypot(NaN,Inf) = Inf (Inf beats NaN)");
    assertNaN(hypot(NaN, 1), "hypot(NaN, finite) = NaN");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
