// covers: dyna:mathx.gcd
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_gcd");
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

__kitS.section("gcd(12,18) = 6n", () => {
    assertEq(gcd(12, 18), 6n, "gcd(12,18) = 6n");
    assert(typeof gcd(12, 18) === "bigint", "gcd returns a BigInt");
    assertEq(gcd(0, 0), 0n, "gcd(0,0) = 0n");
    assertEq(gcd(7, 0), 7n, "gcd(7,0) = 7n");
    assertEq(gcd(0, 7), 7n, "gcd(0,7) = 7n");
    assertEq(gcd(-12, 18), 6n, "gcd(-12,18) = 6n (absolute value)");
    assertEq(gcd(-12, -18), 6n, "gcd(-12,-18) = 6n");
    assertEq(gcd(17, 5), 1n, "gcd(17,5) = 1n (coprime)");
    assertEq(gcd(12n, 18n), 6n, "gcd accepts BigInt args too");

    assertEq(lcm(4, 6), 12n, "lcm(4,6) = 12n");
    assert(typeof lcm(4, 6) === "bigint", "lcm returns a BigInt");
    assertEq(lcm(0, 5), 0n, "lcm(0,5) = 0n");
    assertEq(lcm(5, 0), 0n, "lcm(5,0) = 0n");
    assertEq(lcm(21, 6), 42n, "lcm(21,6) = 42n");
    assertEq(lcm(-4, 6), 12n, "lcm(-4,6) = 12n (absolute value)");

    function gcdRef(a, b) { a = a < 0n ? -a : a; b = b < 0n ? -b : b; while (b) { [a, b] = [b, a % b]; } return a; }
    for (const [a, b] of [[12, 18], [7, 13], [100, 75], [1, 1], [270, 192], [999999937, 2]]) {
        const g = gcdRef(BigInt(a), BigInt(b));
        assertEq(gcd(a, b), g, "gcd matches reference for (" + a + "," + b + ")");
        const want = g === 0n ? 0n : (BigInt(a) * BigInt(b)) / g;
        const l = want < 0n ? -want : want;
        assertEq(lcm(a, b), l, "lcm matches reference for (" + a + "," + b + ")");
    }

    {
        const a = 2n ** 40n, b = 2n ** 40n + 1n;
        const expected = a * b;
        const got = lcm(a, b);
        assertEq(got, expected, "lcm exceeding 64 bits matches pure-JS BigInt reference");
        assert(got > 0xFFFFFFFFFFFFFFFFn, "lcm result genuinely exceeds 2^64-1");
    }
    {
        const a = 6n * 2n ** 40n, b = 10n * 2n ** 40n;
        const g = gcdRef(a, b);
        const expected = (a * b) / g;
        assertEq(lcm(a, b), expected, "lcm with non-trivial gcd, exceeding 64 bits");
    }
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
