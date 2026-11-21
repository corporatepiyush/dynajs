// covers: dyna:mathx.isPrime
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_is_prime");
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

__kitS.section("isPrime(0) = false", () => {
    assertEq(isPrime(0), false, "isPrime(0) = false");
    assertEq(isPrime(1), false, "isPrime(1) = false");
    assertEq(isPrime(2), true, "isPrime(2) = true (smallest prime)");
    assertEq(isPrime(3), true, "isPrime(3) = true");
    assertEq(isPrime(4), false, "isPrime(4) = false");
    assertEq(isPrime(-7), false, "isPrime(-7) = false (negative never prime)");
    assertEq(isPrime(9), false, "isPrime(9) = false (3^2)");
    assertEq(isPrime(17), true, "isPrime(17) = true");
    assertEq(isPrime(97), true, "isPrime(97) = true");
    assertEq(isPrime(100), false, "isPrime(100) = false");

    assertEq(isPrime(1000000007n), true, "isPrime(1e9+7) = true (the classic mod-prime)");
    assertEq(isPrime(1000000009n), true, "isPrime(1e9+9) = true (the other classic mod-prime)");
    assertEq(isPrime(1000000006n), false, "isPrime(1e9+6) = false (even)");

    assertEq(isPrime(2305843009213693951n), true, "isPrime(2^61-1) = true (Mersenne prime M61)");
    assertEq(isPrime(9223372036854775807n), false, "isPrime(2^63-1) = false (composite: divisible by 127)");
    assertEq(9223372036854775807n % 127n, 0n, "sanity: 127 divides 2^63-1");

    assertEq(isPrime(97), isPrime(97n), "isPrime agrees for Number vs BigInt input");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
