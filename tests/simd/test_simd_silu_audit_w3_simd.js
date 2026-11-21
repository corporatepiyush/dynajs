// timeout: 300
// covers: dyna:simd.silu
// tests/simd/test_audit_w3_simd.js -- audit wave 3, dyna:simd value correctness.
// ORACLE for every section: the mathematical definition evaluated in plain JS
// double arithmetic (Math.max, Math.fround(Math.sqrt(x)), a loop that sums in
// double), never another dyna:simd call. CONTROLS are finite, NaN-free inputs
// whose answers were already right. [red] fails on the pre-fix binary on the
// ISA named; unmarked rows are controls or hold on the host ISA already.
//   M1c-01 min/max of all-infinite arrays      M1c-02 distCheb NaN
//   M1c-03 topkIndices with NaN                M1c-04 silu tail differs from body
//   M1c-08 float32 reductions lose 4% at 2e7   M1c-09 x86 silu/vexp overflow
//   M1c-10 NEON vinv/vsqrt/vrsqrt estimates    M1c-12 argmax/argmin/cummax NaN
//   M1c-16 gemv/gemvT/gemm output overlapping an input
// Lengths run 1..130 so every backend's body/tail split (4, 8, 16, 64 lanes)
// is crossed at N-1, N, N+1.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_silu_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("[red] M1c-04: silu of a constant array is one value at every length", () => {
    let bad = 0;
    for (let n = 1; n <= 130; n++) {
        for (const c of [1, -2.5, 0.3]) {
            const a = simd.silu(new Float32Array(n).fill(c));
            for (let i = 1; i < n; i++) if (a[i] !== a[0]) { bad++; break; }
        }
    }
    ok(bad === 0, "[red] M1c-04: silu of a constant array is one value at every length (" + bad + " of 390 arrays mixed two answers)");
});

__kitS.section("[red x86] M1c-09: silu and vexp at the exponent limits", () => {
    let bad = 0, total = 0;
    for (const x of [-1000, -200, -100, -89.5, -3, 0, 2, 88.5, 89.5, 100, 1000]) {
        for (let n = 1; n <= 33; n++) {
            const s = simd.silu(new Float32Array(n).fill(x));
            const want = x / (1 + Math.exp(-x));
            for (let i = 0; i < n; i++) { total++; if (!(Math.abs(s[i] - want) <= 7e-2 * Math.abs(want) + 1e-6)) bad++; }
            if (x >= 89.5 || x <= -200) {
                const e = simd.vexp(new Float32Array(n).fill(x));
                for (let i = 0; i < n; i++) { total++; if (e[i] !== (x > 0 ? Infinity : 0)) bad++; }
            }
        }
    }
    ok(bad === 0, "[red x86] M1c-09: silu and vexp at the exponent limits (" + bad + " of " + total + " elements wrong)");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
