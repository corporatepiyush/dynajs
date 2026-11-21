// covers: dyna:mathx.besselk
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_besselk");
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

__kitS.section("besselk(NaN,1) NaN", () => {
    if (typeof besselk === "function") {
        assertNaN(besselk(NaN, 1), "besselk(NaN,1) NaN");
        assertNaN(besselk(1, NaN), "besselk(1,NaN) NaN");
        assertNaN(besselk(NaN, NaN), "besselk(NaN,NaN) NaN");
        assertNaN(besselkScaled(NaN, 1), "besselkScaled(NaN,1) NaN");
        assertNaN(besselkScaled(1, NaN), "besselkScaled(1,NaN) NaN");
        assertEq(besselk(Infinity, 1), Infinity, "besselk(Inf,1) +Inf (limit in the order)");
        assertEq(besselk(-Infinity, 1), Infinity, "besselk(-Inf,1) +Inf");
        assertEq(besselk(1, Infinity), 0, "besselk(1,Inf) 0");
        assertEq(besselkScaled(Infinity, 1), Infinity, "besselkScaled(Inf,1) +Inf");
        assertEq(besselkScaled(1, Infinity), 0, "besselkScaled(1,Inf) 0");

        assertEq(besselk(0, 0), Infinity, "besselk(0,0) Inf");
        assertNaN(besselk(0, -1), "besselk(0,-1) NaN");
        assertEq(besselk(1e300, 0), Infinity, "besselk(1e300,0) Inf");
        assertEq(besselk(-1e300, 0), Infinity, "besselk(-1e300,0) Inf");
        assertEq(besselk(1e300, 0.25), Infinity, "besselk(1e300,0.25) +Inf");
        assertEq(besselkScaled(1e300, 0.5), Infinity, "besselkScaled(1e300,0.5) +Inf");
        for (const [nu, x] of [[0,1],[0.5,1],[1,1],[2,0.5],[5,2]]) {
            const k = besselk(nu, x);
            const ks = besselkScaled(nu, x);
            assert(Number.isFinite(k), `besselk(${nu},${x}) finite`);
            assert(Number.isFinite(ks), `besselkScaled(${nu},${x}) finite`);
            const expect = k * Math.exp(x);
            const rel = Math.abs(ks - expect) / Math.max(1e-300, Math.abs(expect));
            assert(rel < 1e-12, `besselkScaled vs k*exp at ${nu},${x} rel ${rel}`);
        }
        for (const [nu,x] of [[NaN,1],[1,NaN],[Infinity,1],[1,Infinity],[1e9,1]]) {
            let threw=false; try{besselk(nu,x);}catch(e){threw=true;}
            assert(!threw, `besselk(${nu},${x}) should not throw`);
        }
    }
});
__kitS.end();

print("test_mathx: all tests passed (" + n + " assertions)");
