// covers: dyna:mathx.linspace
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_linspace_matlab");
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

__kitS.section("linspace over integers", () => {
    eqJSON(linspace(0, 1, 5), [0, 0.25, 0.5, 0.75, 1], "linspace(0,1,5)");
    eqJSON(linspace(1, 5, 5), [1, 2, 3, 4, 5], "linspace over integers");
    eqJSON(linspace(0, 1, 1), [1], "linspace with one point is the endpoint");
    eqJSON(linspace(0, 1, 0), [], "linspace with zero points");
    eq(linspace(0, 1, 11)[10], 1, "the final point is exactly b");
    eq(linspace(0, 0.3, 4)[3], 0.3, "final point exact even when the step is not");
    eq(linspace(0, 1, 101).length, 101, "linspace length");
    eqJSON(linspace(5, 1, 5), [5, 4, 3, 2, 1], "linspace descends");

    eqJSON(logspace(0, 3, 4), [1, 10, 100, 1000], "logspace(0,3,4)");
    eq(logspace(0, 2, 3)[2], 100, "logspace endpoint is exact");

    eqJSON(cumsum([1, 2, 3, 4]), [1, 3, 6, 10], "cumsum");
    eqJSON(cumsum([]), [], "cumsum of empty");
    eqJSON(cumsum([5]), [5], "cumsum of one");
    eqJSON(cumprod([1, 2, 3, 4]), [1, 2, 6, 24], "cumprod");
    eqJSON(cumprod([]), [], "cumprod of empty");
    eqJSON(diff([1, 4, 9, 16]), [3, 5, 7], "diff");
    eqJSON(diff([5]), [], "diff of one element is empty");
    eqJSON(diff([]), [], "diff of empty");
    {
        const v = [3, 1, 4, 1, 5, 9, 2, 6];
        eqJSON(diff(cumsum(v)), v.slice(1), "diff(cumsum(v)) == v without its head");
    }
    eqJSON(cumsum(new Float64Array([1, 2, 3])), [1, 3, 6], "cumsum of a Float64Array");
    eqJSON(diff(new Int32Array([10, 7, 3])), [-3, -4], "diff of an Int32Array");
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
