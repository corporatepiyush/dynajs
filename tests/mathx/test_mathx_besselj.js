// covers: dyna:mathx.besselj
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_besselj");
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

__kitS.section("a huge order underflows to 0", () => {
    const t0 = cpuNow();
    assertEq(besselj(2147483647, 3), 0, "a huge order underflows to 0");
    assertEq(besselj(-2147483648, 1), 0, "and so does INT_MIN, whose negation is itself");
    assert(cpuNow() - t0 < 1000,
           "a huge order returns without running the recurrence (took " +
           (cpuNow() - t0) + " ms)");

    assertEq(besselj(50, 10), 1.784513607871594e-30, "an order libm can still reach is unchanged");
    assert(besselj(200, 10) !== 0, "6.9e-236 is representable, so it is not zeroed");
    assert(besselj(200, 100) !== 0, "nor is 2.1e-41");

    let worst = 0;
    for (const x of [0.5, 1, 2.5, 3, 7, 10, 25, 100])
        for (let k = 1; k <= 120; k++) {
            const lhs = besselj(k - 1, x) + besselj(k + 1, x);
            const rhs = (2 * k / x) * besselj(k, x);
            worst = Math.max(worst, Math.abs(lhs - rhs) / Math.max(1e-300, Math.abs(rhs)));
        }
    assert(worst < 1e-12, "960 orders satisfy the Bessel recurrence (worst " + worst + ")");

    assertEq(besselj(-3, 2.5), -besselj(3, 2.5), "odd negative order flips sign");
    assertEq(besselj(-4, 2.5), besselj(4, 2.5), "even negative order does not");
    let refused = false;
    try { besselj(2147483647, 2147483647); } catch (e) { refused = e instanceof RangeError; }
    assert(refused, "an order that would recurse for seconds is refused");
    assertEq(besselj(2147483647, 3), 0, "the same order with a small x still answers");
    assert(besselj(16777216, 1e300) !== undefined, "the cap itself is allowed");

    assertEq(besselj(5, 0), 0, "J_n(0) is 0 for n >= 1");
    assertEq(besselj(0, 0), 1, "and J_0(0) is 1");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
