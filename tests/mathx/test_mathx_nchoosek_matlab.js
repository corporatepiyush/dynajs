// covers: dyna:mathx.nchoosek
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_nchoosek_matlab");
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

__kitS.section("factor of a prime", () => {
    eqJSON(factor(360), [2, 2, 2, 3, 3, 5], "factor(360)");
    eqJSON(factor(2), [2], "factor of a prime");
    eqJSON(factor(1), [], "factor(1) is empty");
    eqJSON(factor(97), [97], "factor of a larger prime");
    eqJSON(factor(1024), [2, 2, 2, 2, 2, 2, 2, 2, 2, 2], "factor(1024)");
    for (const v of [12, 100, 999, 65536, 123456])
        eq(factor(v).reduce((a, b) => a * b, 1), v, "factors multiply back to " + v);
    throwsRange(() => factor(0), "factor(0)");
    throwsRange(() => factor(-4), "factor of a negative");
    throwsRange(() => factor(2.5), "factor of a non-integer");

    eqJSON(primes(20), [2, 3, 5, 7, 11, 13, 17, 19], "primes(20)");
    eqJSON(primes(2), [2], "primes(2)");
    eqJSON(primes(1), [], "primes(1)");
    eqJSON(primes(0), [], "primes(0)");
    eq(primes(100).length, 25, "25 primes below 100");
    eq(primes(1000).length, 168, "168 primes below 1000");
    for (const p of primes(200))
        eq(factor(p).length, 1, p + " is prime");

    eq(nchoosek(5, 2), 10, "nchoosek(5,2)");
    eq(nchoosek(52, 5), 2598960, "nchoosek(52,5)");
    eq(nchoosek(10, 0), 1, "nchoosek(n,0)");
    eq(nchoosek(10, 10), 1, "nchoosek(n,n)");
    eq(nchoosek(3, 5), 0, "k>n is 0");
    eq(nchoosek(40, 20), 137846528820, "nchoosek(40,20) is exact past 2^32");
    for (let k = 0; k <= 9; k++)
        eq(nchoosek(9, k), nchoosek(9, 9 - k), "nchoosek symmetry at k=" + k);
    for (let k = 1; k <= 8; k++)
        eq(nchoosek(9, k), nchoosek(8, k - 1) + nchoosek(8, k), "Pascal's rule at k=" + k);
    throwsRange(() => nchoosek(-1, 2), "negative n");
    throwsRange(() => nchoosek(5, 1.5), "non-integer k");

    eqJSON(rat(0.75), [3, 4], "rat(0.75)");
    eqJSON(rat(0.5), [1, 2], "rat(0.5)");
    eqJSON(rat(2), [2, 1], "rat of an integer");
    eqJSON(rat(-0.25), [-1, 4], "rat keeps the sign on the numerator");
    {
        const [p, q] = rat(Math.PI, 1e-6);
        close(p / q, Math.PI, 1e-6, "rat(pi) is within tolerance");
        assert(q > 0, "rat denominator is positive");
    }
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
