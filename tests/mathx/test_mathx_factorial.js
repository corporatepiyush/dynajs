// covers: dyna:mathx.factorial
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_factorial");
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

__kitS.section("factorial(0) = 1n", () => {
    assertEq(factorial(0), 1n, "factorial(0) = 1n");
    assertEq(factorial(1), 1n, "factorial(1) = 1n");
    assertEq(factorial(5), 120n, "factorial(5) = 120n");
    assertEq(factorial(10), 3628800n, "factorial(10) = 3628800n");
    assert(typeof factorial(5) === "bigint", "factorial returns a BigInt");

    assertEq(factorial(20), 2432902008176640000n, "factorial(20) known value");

    function factorialRef(n) {
        let r = 1n;
        for (let i = 2n; i <= BigInt(n); i++) r *= i;
        return r;
    }
    for (const k of [2, 6, 19, 20, 21, 22, 25, 30, 40, 50, 64, 99, 100, 170, 250, 500, 999])
        assertEq(factorial(k), factorialRef(k), "factorial(" + k + ") matches pure-JS BigInt reference");

    let threw;
    threw = false; try { factorial(-1); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(-1) throws RangeError");
    threw = false; try { factorial(3.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(3.5) throws RangeError (non-integer)");
    threw = false; try { factorial(100000000); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "factorial(1e8) throws RangeError (over the compute cap)");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
