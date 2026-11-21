// covers: dyna:mathx.nextpow2
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_nextpow2_matlab");
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

__kitS.section("fix truncates toward zero", () => {
    eq(fix(2.7), 2, "fix truncates toward zero");
    eq(fix(-2.7), -2, "fix(-2.7)");
    eq(fix(-0.5), -0, "fix keeps -0");
    eq(fix(2.7), trunc(2.7), "fix is trunc");

    eq(sign(3), 1, "sign(+)"); eq(sign(-3), -1, "sign(-)");
    eq(sign(0), 0, "sign(0)");
    eq(sign(-0), -0, "sign(-0) preserves -0");
    assert(Number.isNaN(sign(NaN)), "sign(NaN) is NaN");

    close(nthroot(-8, 3), -2, 1e-15, "nthroot of a negative with an odd root");
    close(nthroot(27, 3), 3, 1e-15, "nthroot(27,3)");
    close(nthroot(16, 4), 2, 1e-15, "nthroot(16,4)");
    assert(Number.isNaN(nthroot(-16, 4)), "even root of a negative is NaN");
    assert(Number.isNaN(Math.pow(-8, 1 / 3)), "pow cannot do it, which is why nthroot exists");

    eq(nextpow2(0), 0, "nextpow2(0)");
    eq(nextpow2(1), 0, "nextpow2(1)");
    eq(nextpow2(2), 1, "nextpow2(2)");
    eq(nextpow2(5), 3, "nextpow2(5)");
    eq(nextpow2(1024), 10, "nextpow2(1024)");
    eq(nextpow2(-5), 3, "nextpow2 uses |x|");
    eq(pow2(10), 1024, "pow2(10)");
    close(pow2(0.5), Math.SQRT2, 1e-15, "pow2(0.5)");

    close(deg2rad(180), Math.PI, 1e-15, "deg2rad(180)");
    close(rad2deg(Math.PI), 180, 1e-13, "rad2deg(pi)");
    close(rad2deg(deg2rad(37)), 37, 1e-13, "deg2rad/rad2deg round-trip");
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
