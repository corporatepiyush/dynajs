// timeout: 300
// covers: dyna:mathx.gammainc
// tests/mathx/test_audit_w3_mathx.js -- audit wave 3, dyna:mathx value correctness.
// Every expected value comes from OUTSIDE the code under test:
//   - the DEFINITION, summed in plain JS: for integer a the regularized upper
//     incomplete gamma is the Poisson CDF, Q(a, x) = sum_{k<a} e^-x x^k / k!
//     (DLMF 8.4.10); I_n(x) = sum_k (x/2)^(2k+n) / (k! (k+n)!) (DLMF 10.25.2).
//     Both are summed in the log domain with log k! built by adding logs.
//   - an exact symmetry: I_{1/2}(a, a) = 1/2 (DLMF 8.17.4 with x = 1/2).
//   - published values: I_0(1) = 1.2660658777520084 (Abramowitz & Stegun
//     Table 9.8); P(a, a) ~ 1/2 + 1/(3 sqrt(2 pi a)) (DLMF 8.11.12).
//   - libm's forward erfc for the inverse (a different implementation from
//     the inverse under test).
// [red] fails on the pre-fix binary.
//   M1c-20 gammainc/betainc truncated after 400 terms
//   M1c-21 besseli negative garbage for x > 20 with a moderate order
//   M1c-22 scalbn/ldexp exponent wrap, erfcinv near 0, legendreP order,
//          besselk/bessely order edges
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_gammainc_audit_w3_mathx");
import * as mx from "dyna:mathx";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(b), 1e-300);

const LOGFACT = [0];
function logFact(n) { for (let k = LOGFACT.length; k <= n; k++) LOGFACT.push(LOGFACT[k - 1] + Math.log(k)); return LOGFACT[n]; }

// ---- M1c-20 -------------------------------------------------------------

__kitS.section("[red]", () => {
    function poissonQ(a, x) {
        let s = 0;
        const lx = Math.log(x);
        for (let k = 0; k < a; k++) s += Math.exp(-x + k * lx - logFact(k));
        return s;
    }
    for (const [a, x] of [[10, 7], [1000, 1000], [3000, 3100], [10000, 10000], [100000, 100000], [100000, 99500], [200000, 200300]]) {
        const q = poissonQ(a, x);
        const up = mx.gammainc(x, a, "upper"), lo = mx.gammainc(x, a);
        const tag = a >= 3000 ? "[red] " : "control: ";
        // The oracle's own precision: each term is exp() of a difference of
        // numbers near a*ln(a) (1.15e6 at a = 1e5), so the plain-double sum
        // is good to about 1e-9 there; 2e-8 still separates it from the
        // truncated series, which was off by 0.1.
        const tol = a >= 1e5 ? 2e-8 : 1e-9;
        ok(rel(up, q) < tol, tag + "M1c-20: Q(" + a + ", " + x + ") = " + up + ", Poisson sum " + q);
        ok(rel(lo, 1 - q) < tol, tag + "M1c-20: P(" + a + ", " + x + ") = " + lo + ", definition " + (1 - q));
    }
    for (const a of [1e5, 1e6, 1e7]) {
        const p = mx.gammainc(a, a), ref = 0.5 + 1 / (3 * Math.sqrt(2 * Math.PI * a));
        ok(Math.abs(p - ref) < 2 / a, "[red] M1c-20: P(a, a) at a=" + a + " is " + p + " (DLMF 8.11.12: " + ref + ")");
    }
    for (const a of [100, 1e4, 1e6, 1e7, 1e8]) {
        const v = mx.betainc(0.5, a, a);
        ok(Math.abs(v - 0.5) < 1e-6, (a >= 1e7 ? "[red] " : "control: ") + "M1c-20: betainc(0.5, a, a) at a=" + a + " is " + v);
    }
    const med = mx.gammaincinv(0.5, 1e5);
    ok(Math.abs(med - (1e5 - 1 / 3)) < 0.01, "[red] M1c-20: median of Gamma(1e5) is a - 1/3 + O(1/a) (" + med + ")");
});
__kitS.end();

print("test_audit_w3_mathx: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_mathx: " + failures + " failures");
