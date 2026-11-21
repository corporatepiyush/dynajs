// covers: dyna:mathx.modf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_modf");
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

__kitS.section("modf(3.75) intPart", () => {
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
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
