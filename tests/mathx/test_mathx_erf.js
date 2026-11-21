// covers: dyna:mathx.erf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_erf");
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

__kitS.section("erf(0) = 0", () => {
    assertEq(erf(0), 0, "erf(0) = 0");
    assertEq(erf(Infinity), 1, "erf(+Inf) = 1");
    assertEq(erf(-Infinity), -1, "erf(-Inf) = -1");
    assertNaN(erf(NaN), "erf(NaN) = NaN");

    assertEq(erfc(0), 1, "erfc(0) = 1");
    assertEq(erfc(Infinity), 0, "erfc(+Inf) = 0");
    assertEq(erfc(-Infinity), 2, "erfc(-Inf) = 2");
    assertNaN(erfc(NaN), "erfc(NaN) = NaN");

    for (const x of [0.1, 0.5, 1, 1.5, 2, 3, -0.7, -2.2])
        assertClose(erf(x) + erfc(x), 1, 1e-12, "erf+erfc==1 at x=" + x);
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
