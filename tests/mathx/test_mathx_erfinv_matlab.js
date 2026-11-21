// covers: dyna:mathx.erfinv
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

{
    const LGAMMA = [[0.5, 0.5723649429247004], [1, 0.0], [1.5, -0.12078223763524543],
                    [2, 0.0], [3, 0.693147180559945], [4.5, 2.4537365708424432],
                    [10, 12.801827480081467], [100, 359.1342053695754]];
    for (const [x, want] of LGAMMA) {
        close(gammaln(x), want, 1e-14, "gammaln(" + x + ")");
        eq(gammaln(x), lgamma(x)[0], "gammaln == lgamma value for " + x);
    }

    const PSI = [[0.5, -1.9635100260214235], [1, -0.5772156649015329],
                 [1.5, 0.03648997397857652], [2, 0.42278433509846713],
                 [3, 0.9227843350984671], [3.7, 1.1671535393615113],
                 [5, 1.5061176684318005], [10, 2.251752589066721],
                 [100, 4.600161852738087]];
    for (const [x, want] of PSI)
        close(psi(x), want, 1e-13, "psi(" + x + ")");
    for (const x of [0.3, 1.1, 2.5, 7.9])
        close(psi(x + 1), psi(x) + 1 / x, 1e-12, "psi recurrence at " + x);
    close(psi(1.5) - psi(-0.5), Math.PI / Math.tan(Math.PI * -0.5), 1e-10,
          "psi reflection formula");
    assert(Number.isNaN(psi(-1)), "psi has a pole at a negative integer");
    assert(Number.isNaN(psi(-2)), "psi(-2) is a pole");

    const BETA = [[2, 3, 0.0833333333333334], [0.5, 0.5, 3.1415926535897953],
                  [1, 1, 1.0], [5, 7, 0.00043290043290043176],
                  [0.1, 0.9, 10.166407384630517]];
    for (const [a, b, want] of BETA) {
        close(beta(a, b), want, 1e-13, `beta(${a},${b})`);
        close(Math.exp(betaln(a, b)), want, 1e-12, `exp(betaln(${a},${b}))`);
    }
    eq(beta(2, 3), beta(3, 2), "beta is symmetric");
    assert(Number.isFinite(betaln(1e-4, 1e-4)), "betaln survives tiny arguments");

    for (const x of [0.1, 0.5, 0.7, 1.0, 1.5, 2.0, -0.3, -1.2]) {
        close(erfinv(erf(x)), x, 1e-12, "erfinv(erf(" + x + ")) round-trips");
        close(erf(erfinv(Math.tanh(x))), Math.tanh(x), 1e-13, "erf(erfinv(y)) at " + x);
    }
    eq(erfinv(0), 0, "erfinv(0)");
    eq(erfinv(1), Infinity, "erfinv(1)");
    eq(erfinv(-1), -Infinity, "erfinv(-1)");
    assert(Number.isNaN(erfinv(1.5)), "erfinv outside [-1,1] is NaN");
    for (const y of [0.01, 0.5, 0.99, 1.5, 1.99])
        close(erfcinv(y), erfinv(1 - y), 1e-12, "erfcinv(y) == erfinv(1-y) at " + y);

    for (const x of [0, 0.5, 1, 2, 5])
        close(erfcx(x), Math.exp(x * x) * erfc(x), 1e-12, "erfcx(" + x + ")");
    eq(erfc(40), 0, "erfc underflows to 0 by x=40");
    assert(erfcx(40) > 0 && Number.isFinite(erfcx(40)),
           "erfcx stays finite and positive where erfc underflowed");
    close(erfcx(40), 1 / (40 * Math.sqrt(Math.PI)), 1e-3, "erfcx(40) ~ 1/(x*sqrt(pi))");

    const E1 = [[0.1, 1.8229239584193906], [0.5, 0.5597735947761608],
                [1, 0.21938393439552029], [2, 0.04890051070806112],
                [5, 0.0011482955912753257], [10, 4.156968929685325e-06]];
    for (const [x, want] of E1)
        close(expint(x), want, 1e-12, "expint(" + x + ")");
    eq(expint(0), Infinity, "expint(0)");
    assert(Number.isNaN(expint(-1)), "expint is complex for x<0, so NaN");
}

print("test_mathx_matlab: all " + n + " tests passed");
