// covers: dyna:mathx.lgamma
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_lgamma");
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

__kitS.section(") is log(", () => {
    // At a positive integer up to 23, (n-1)! is exact in a double, so
    // lgamma(n) is log of an exact integer: one rounding, the same on every
    // platform. The C library's lgamma is not that accurate everywhere
    // (macOS: 1 ulp low at 5), which is why these are computed, not looked up.
    let f = 1n;
    for (let k = 1; k <= 23; k++) {
        if (k > 1) f *= BigInt(k - 1);
        const want = Math.log(Number(f));
        const [v, s] = lgamma(k);
        assert(Object.is(v, want) && s === 1, "lgamma(" + k + ") is log(" + (k - 1) + "!) exactly: " + v + " vs " + want);
        assert(Object.is(gammaln(k), want), "gammaln(" + k + ") is log(" + (k - 1) + "!) exactly");
    }
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
