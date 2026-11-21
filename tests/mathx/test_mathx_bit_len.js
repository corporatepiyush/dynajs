// covers: dyna:mathx.bitLen
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_bit_len");
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

__kitS.section("abs(5n) = 5n", () => {
    assertEq(abs(5n), 5n, "abs(5n) = 5n");
    assertEq(abs(-5n), 5n, "abs(-5n) = 5n");
    assertEq(abs(0n), 0n, "abs(0n) = 0n");
    assertEq(abs(-9223372036854775808n), 9223372036854775808n,
        "abs(INT64_MIN) = 2^63 (the one magnitude with no positive int64 twin)");

    let threw = false;
    try { abs(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "abs(Number) throws TypeError -- BigInt required");

    assertEq(bitLen(0n), 0, "bitLen(0n) = 0");
    assertEq(bitLen(1n), 1, "bitLen(1n) = 1");
    assertEq(bitLen(255n), 8, "bitLen(255n) = 8 (0b11111111)");
    assertEq(bitLen(256n), 9, "bitLen(256n) = 9 (0b100000000)");
    assertEq(bitLen(-255n), 8, "bitLen(-255n) = 8 (operates on magnitude)");
    assertEq(bitLen(-9223372036854775808n), 64, "bitLen(INT64_MIN) = 64 (|INT64_MIN| = 2^63)");

    assertEq(popcount(0n), 0, "popcount(0n) = 0");
    assertEq(popcount(255n), 8, "popcount(255n) = 8 (all bits set)");
    assertEq(popcount(256n), 1, "popcount(256n) = 1 (single bit)");
    assertEq(popcount(-255n), 8, "popcount(-255n) = 8 (operates on magnitude)");
    assertEq(popcount(-9223372036854775808n), 1, "popcount(INT64_MIN) = 1 (|INT64_MIN| = 2^63, one bit)");

    threw = false;
    try { bitLen(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "bitLen(Number) throws TypeError -- BigInt required");
    threw = false;
    try { popcount(5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "popcount(Number) throws TypeError -- BigInt required");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
