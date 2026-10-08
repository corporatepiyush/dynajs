// timeout: 300
// tests/test_audit_w3_mathx.js -- audit wave 3, dyna:mathx value correctness.
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
import * as mx from "dyna:mathx";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const rel = (a, b) => Math.abs(a - b) / Math.max(Math.abs(b), 1e-300);

const LOGFACT = [0];
function logFact(n) { for (let k = LOGFACT.length; k <= n; k++) LOGFACT.push(LOGFACT[k - 1] + Math.log(k)); return LOGFACT[n]; }

// ---- M1c-20 -------------------------------------------------------------
{
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
}

// ---- M1c-21 -------------------------------------------------------------
{
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
}

// ---- M1c-22 -------------------------------------------------------------
{
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
}

print("test_audit_w3_mathx: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_mathx: " + failures + " failures");
