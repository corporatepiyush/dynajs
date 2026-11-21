// timeout: 300
// covers: dyna:mathx.erfcinv
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
const __kitS = __kitSections("test_mathx_erfcinv_audit_w3_mathx");
import * as mx from "dyna:mathx";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(b), 1e-300);

const LOGFACT = [0];
function logFact(n) { for (let k = LOGFACT.length; k <= n; k++) LOGFACT.push(LOGFACT[k - 1] + Math.log(k)); return LOGFACT[n]; }

// ---- M1c-20 -------------------------------------------------------------

__kitS.section("[red] scalbn with an exponent beyond int32 saturates", () => {
    ok(mx.scalbn(1, 2 ** 40) === Infinity && mx.scalbn(1, -(2 ** 40)) === 0, "[red] scalbn with an exponent beyond int32 saturates (" + mx.scalbn(1, 2 ** 40) + ")");
    ok(mx.scalbn(1, 2 ** 32 + 1) === Infinity, "[red] scalbn(1, 2^32+1) is Infinity, not 2");
    ok(mx.ldexp(1, 2 ** 40) === Infinity, "[red] ldexp with an exponent beyond int32 saturates");
    ok(mx.scalbn(3, 4) === 48 && mx.ldexp(0.75, -1) === 0.375, "control: ordinary scalbn/ldexp");
    for (const y of [1e-3, 1e-10, 1e-17, 1e-20, 1e-100, 1e-300]) {
        const x = mx.erfcinv(y);
        ok(Number.isFinite(x) && rel(mx.erfc(x), y) < 1e-12, (y <= 1e-10 ? "[red] " : "control: ") + "erfc(erfcinv(" + y + ")) round-trips through libm erfc (x=" + x + ")");
    }
    ok(mx.erfcinv(1) === 0 && mx.erfcinv(0) === Infinity && mx.erfcinv(2) === -Infinity, "control: erfcinv at 1, 0 and 2");
    ok(rel(mx.erfcinv(1.999), -mx.erfcinv(0.001)) < 1e-14, "erfcinv(2 - y) = -erfcinv(y)");
    // The tested contract (test_mathx_tierb) is NaN for an order above the
    // degree; the defect was converting 1e300 to int first (undefined
    // behaviour, caught only by the sanitizer leg), so this row is a control
    // on a release build.
    ok(Number.isNaN(mx.legendreP(3, 1e300, 0.5)) && Number.isNaN(mx.legendreP(3, 4, 0.5)), "legendreP with order above degree is NaN");
    ok(rel(mx.legendreP(2, 0, 0.5), -0.125) < 1e-15, "control: P_2(0.5) = -1/8");
    ok(mx.besselk(1e6 + 1, 0.4) === Infinity, "[red] K of a huge order at small x is +Infinity, not 0");
    let err = "none";
    try { mx.bessely(2 ** 25, 5); } catch (e) { err = e.constructor.name; }
    ok(err === "RangeError", "[red] bessely refuses an order above 2^24 like besselj (" + err + ")");
});
__kitS.end();

print("test_audit_w3_mathx: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_mathx: " + failures + " failures");
