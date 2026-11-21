// covers: dyna:mathx.mod
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_mod_matlab");
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

__kitS.section("mod(-1,3) is floored", () => {
    eq(mod(5, 3), 2, "mod(5,3)");
    eq(rem(5, 3), 2, "rem(5,3)");
    eq(mod(-1, 3), 2, "mod(-1,3) is floored");
    eq(rem(-1, 3), -1, "rem(-1,3) is truncated");
    eq(fmod(-1, 3), -1, "fmod agrees with rem, not mod");
    eq(mod(1, -3), -2, "mod(1,-3) takes the divisor's sign");
    eq(rem(1, -3), 1, "rem(1,-3) takes the dividend's sign");
    eq(mod(-5, -3), -2, "mod(-5,-3)");
    eq(rem(-5, -3), -2, "rem(-5,-3)");
    for (const [a, b] of [[7, 3], [-7, 3], [7, -3], [-7, -3]]) {
        const m1 = mod(a, b), r1 = rem(a, b);
        assert(m1 === 0 || Math.sign(m1) === Math.sign(b), `mod(${a},${b}) sign follows divisor`);
        assert(r1 === 0 || Math.sign(r1) === Math.sign(a), `rem(${a},${b}) sign follows dividend`);
    }
    eq(mod(5, 0), 5, "mod by zero returns the dividend (MATLAB)");
    assert(Number.isNaN(rem(5, 0)), "rem by zero is NaN (C fmod)");
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
