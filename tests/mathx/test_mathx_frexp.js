// covers: dyna:mathx.frexp
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_frexp");
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

__kitS.section("scalbn(1,10) = 1024", () => {
    assertEq(scalbn(1, 10), 1024, "scalbn(1,10) = 1024");
    assertEq(ldexp(1, 10), 1024, "ldexp(1,10) = 1024");
    assertEq(scalbn(3, -2), 0.75, "scalbn(3,-2) = 0.75");
    assertEq(ldexp(1, 10), scalbn(1, 10), "ldexp and scalbn agree (FLT_RADIX==2)");

    for (const x of [8, 1, 0.5, 3.14159, -12345.6789, 1e300, 1e-300]) {
        const [frac, exp] = frexp(x);
        assertClose(ldexp(frac, exp), x, Math.abs(x) * 1e-15 || 1e-300,
            "ldexp(frexp(x)) roundtrips for x=" + x);
        if (x !== 0)
            assert(Math.abs(frac) >= 0.5 && Math.abs(frac) < 1,
                "frexp fraction in [0.5,1) for x=" + x);
    }
    {
        const [frac, exp] = frexp(8);
        assertEq(frac, 0.5, "frexp(8) frac = 0.5");
        assertEq(exp, 4, "frexp(8) exp = 4");
    }
    {
        const [frac, exp] = frexp(0);
        assertPosZero(frac, "frexp(+0) frac = +0");
        assertEq(exp, 0, "frexp(+0) exp = 0");
    }
    {
        const [frac, exp] = frexp(-0);
        assertNegZero(frac, "frexp(-0) frac = -0");
        assertEq(exp, 0, "frexp(-0) exp = 0");
    }
    {
        const [frac, exp] = frexp(Infinity);
        assertEq(frac, Infinity, "frexp(+Inf) frac = +Inf");
        assertEq(exp, 0, "frexp(+Inf) exp = 0");
    }
    {
        const [frac] = frexp(NaN);
        assertNaN(frac, "frexp(NaN) frac = NaN");
    }
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
