// timeout: 300
// covers: dyna:mathx.besseli
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
const __kitS = __kitSections("test_mathx_besseli_audit_w3_mathx");
import * as mx from "dyna:mathx";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(b), 1e-300);

const LOGFACT = [0];
function logFact(n) { for (let k = LOGFACT.length; k <= n; k++) LOGFACT.push(LOGFACT[k - 1] + Math.log(k)); return LOGFACT[n]; }

// ---- M1c-20 -------------------------------------------------------------

__kitS.section("control: I_0(1) matches A&S Table 9.8", () => {
    function besselIdef(n, x) {
        let s = 0;
        const lh = Math.log(x / 2);
        for (let k = 0; k < 4000; k++) {
            const t = Math.exp((2 * k + n) * lh - logFact(k) - logFact(k + n));
            s += t;
            if (k > x && t < s * 1e-18) break;
        }
        return s;
    }
    ok(rel(mx.besseli(0, 1), 1.2660658777520084) < 1e-14, "control: I_0(1) matches A&S Table 9.8 (" + mx.besseli(0, 1) + ")");
    for (const [n, x] of [[0, 25], [5, 25], [10, 19.5], [10, 20.5], [30, 21], [30, 25], [50, 25], [100, 25], [100, 50], [20, 30], [3, 400], [60, 300]]) {
        const got = mx.besseli(n, x), want = besselIdef(n, x);
        const wasWrong = x > 20 && 4 * n * n - 1 > 4 * x;
        ok(rel(got, want) < 1e-11, (wasWrong ? "[red] " : "control: ") + "M1c-21: I_" + n + "(" + x + ") = " + got + ", definition " + want);
    }
    ok(rel(mx.besseliScaled(60, 40), besselIdef(60, 40) * Math.exp(-40)) < 1e-11, "[red] M1c-21: besseliScaled(60, 40) = " + mx.besseliScaled(60, 40));
    let bad = 0;
    for (const n of [0, 5, 10, 30, 100]) {
        for (let x = 0.5; x <= 120; x += 0.5) if (!(mx.besseli(n, x) > 0)) bad++;
        const a = mx.besseli(n, 19.999), b = mx.besseli(n, 20.001);
        // d/dx ln I_n(x) is below n/x + 1, i.e. 6 at n = 100, x = 20: over the
        // 0.002 step the value may grow by up to ~1.2%; the old discontinuity
        // was a sign flip.
        if (!(b > a) || rel(b, a) > 0.02) bad++;
    }
    ok(bad === 0, "[red] M1c-21: I_n(x) > 0 on (0, 120] and increasing across x = 20 for n in {0,5,10,30,100} (" + bad + " violations)");
});
__kitS.end();

print("test_audit_w3_mathx: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_mathx: " + failures + " failures");
