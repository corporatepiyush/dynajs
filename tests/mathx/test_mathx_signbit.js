// covers: dyna:mathx.signbit
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_signbit");
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

__kitS.section("copysign(3,-1) = -3", () => {
    assertEq(copysign(3, -1), -3, "copysign(3,-1) = -3");
    assertEq(copysign(-3, 1), 3, "copysign(-3,1) = 3");
    assertEq(copysign(3, -0), -3, "copysign(3,-0) = -3 (sign of -0 is negative)");
    assertEq(copysign(3, 0), 3, "copysign(3,+0) = 3");
    assertNaN(copysign(NaN, -1), "copysign(NaN,-1) is still NaN");

    assert(signbit(-1) === true, "signbit(-1)");
    assert(signbit(1) === false, "signbit(1) === false");
    assert(signbit(-0) === true, "signbit(-0)");
    assert(signbit(0) === false, "signbit(+0) === false");
    assert(signbit(-Infinity) === true, "signbit(-Inf)");
    assert(signbit(Infinity) === false, "signbit(+Inf) === false");
    assert(signbit(NaN) === false, "signbit(NaN) observed false on this engine/platform");
    assert(signbit(-NaN) === true, "signbit(-NaN) observed true (sign bit flipped by unary -)");

    assert(nextafter(1, 2) > 1, "nextafter(1,2) > 1");
    assert(nextafter(1, 0) < 1, "nextafter(1,0) < 1");
    assertEq(nextafter(5, 5), 5, "nextafter(x,x) = x");
    assertEq(nextafter(0, 1), Number.MIN_VALUE, "nextafter(0,1) = smallest denormal");
    assertNaN(nextafter(NaN, 1), "nextafter(NaN,y) = NaN");
    assertNaN(nextafter(1, NaN), "nextafter(x,NaN) = NaN");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
