// covers: dyna:mathx.eps
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_eps_matlab");
import { mod, rem, fmod, fix, sign, nthroot, nextpow2, pow2, deg2rad, rad2deg, eps, realmin, realmax, flintmax, beta, betaln, gammaln, psi, erfinv, erfcinv, erfcx, expint, factor, primes, nchoosek, rat, linspace, logspace, cumsum, cumprod, diff, erf, erfc, lgamma, trunc, bits } from "dyna:mathx";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (!Object.is(got, want))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
function close(got, want, tol, m) {
    n++;
    const err = Math.abs(got - want);
    const rel = want === 0 ? err : err / Math.abs(want);
    if (!(err <= tol || rel <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want +
            "\n  abs err " + err.toExponential(2) + ", rel " + rel.toExponential(2) +
            " (tol " + tol + ")");
}
function eqJSON(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + m + "\n  got:  " + a + "\n  want: " + b);
}
function throwsRange(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}

__kitS.section("bare eps() is eps(1) = 2^-52", () => {
    eq(eps(), Number.EPSILON, "bare eps() is eps(1) = 2^-52");
    eq(eps(1), Number.EPSILON, "eps(1)");
    for (const x of [1, 2, 0.5, 1e10, 1e-10, 12345.678]) {
        const e = eps(x);
        eq(x + e > x, true, "eps(" + x + ") actually reaches the next double");
        eq(x + e / 2 === x || x + e / 2 === x + e, true, "eps(" + x + ") is minimal");
    }
    eq(eps(-3), eps(3), "eps uses |x|");
    eq(eps(5e-324), 5e-324, "eps of a subnormal is the subnormal step");
    assert(Number.isNaN(eps(NaN)), "eps(NaN)");
    assert(Number.isNaN(eps(Infinity)), "eps(Infinity)");
    eq(realmin(), 2.2250738585072014e-308, "realmin is DBL_MIN");
    eq(realmax(), Number.MAX_VALUE, "realmax is DBL_MAX");
    eq(flintmax(), 9007199254740992, "flintmax is 2^53");
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
