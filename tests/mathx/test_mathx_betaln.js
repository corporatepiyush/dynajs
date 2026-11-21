// covers: dyna:mathx.betaln
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_betaln");
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

__kitS.section("betaln(-2.5,1.5): Gamma(x+y) pole -> -Inf", () => {
    for (const [x, y] of [[2.5, 1.5], [-2.5, 5.5], [-0.5, 2.5], [4.5, -1.5]]) {
        const ref = gammaln(x) + gammaln(y) - gammaln(x + y);
        const got = betaln(x, y);
        const rel = Math.abs(got - ref) / Math.abs(ref);
        assert(rel < 1e-12, `betaln(${x},${y}) == gammaln identity (rel ${rel})`);
    }

    assertEq(betaln(-2.5, 1.5), -Infinity, "betaln(-2.5,1.5): Gamma(x+y) pole -> -Inf");
    assertEq(gammaln(-2.5) + gammaln(1.5) - gammaln(-1), -Infinity,
             "the identity's other side at the same pole");

    assertEq(betaln(-1, 0.5), Infinity, "betaln(-1,0.5): pole -> +Inf");
    assertEq(beta(-1, 0.5), Infinity, "beta(-1,0.5): pole -> +Inf");

    assertEq(beta(-2.5, 5.5), -Math.exp(betaln(-2.5, 5.5)),
             "beta(-2.5,5.5) === -exp(betaln(-2.5,5.5)): betaln is log|B|");

    assertClose(betaln(2, 3), Math.log(beta(2, 3)), 1e-12, "betaln(2,3) == log(beta(2,3))");
    assertNaN(betaln(NaN, 1), "betaln(NaN,1) stays NaN");
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
