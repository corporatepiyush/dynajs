// covers: dyna:mathx.gamma
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_gamma");
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

__kitS.section("gamma(5) = 4! = 24", () => {
    assertEq(gamma(5), 24, "gamma(5) = 4! = 24");
    assertEq(gamma(1), 1, "gamma(1) = 0! = 1");
    assertEq(gamma(6), 120, "gamma(6) = 5! = 120");
    assertClose(gamma(0.5), Math.sqrt(Pi), 1e-14, "gamma(0.5) ~ sqrt(pi)");

    assertEq(gamma(Infinity), Infinity, "gamma(+Inf) = +Inf");
    assertNaN(gamma(-Infinity), "gamma(-Inf) = NaN");
    assertEq(gamma(0), Infinity, "gamma(+0) = +Inf");
    assertEq(gamma(-0), -Infinity, "gamma(-0) = -Inf");
    assertNaN(gamma(-1), "gamma(-1) = NaN (negative integer)");
    assertNaN(gamma(-2), "gamma(-2) = NaN (negative integer)");
    assertNaN(gamma(NaN), "gamma(NaN) = NaN");

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
        const [v] = lgamma(-3);
        assertEq(v, Infinity, "lgamma(-3) value = +Inf (pole)");
    }
    {
        const [v] = lgamma(NaN);
        assertNaN(v, "lgamma(NaN) value = NaN");
    }
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
