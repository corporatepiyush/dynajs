// timeout: 300
// covers: dyna:simd.topkIndices
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
const __kitS = __kitSections("test_simd_topk_indices_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("[red] M1c-03: top-1 of [NaN,1..5] is index 5", () => {
    const v = Float32Array.of(NaN, 1, 2, 3, 4, 5);
    ok(Array.from(simd.topkIndices(v, 1)).join() === "5", "[red] M1c-03: top-1 of [NaN,1..5] is index 5 (" + Array.from(simd.topkIndices(v, 1)) + ")");
    ok(Array.from(simd.topkIndices(v, 2)).sort().join() === "4,5", "[red] M1c-03: top-2 is {4,5} (" + Array.from(simd.topkIndices(v, 2)) + ")");
    const rnd = lcg(7);
    let bad = 0;
    for (let trial = 0; trial < 200; trial++) {
        const n = 5 + (rnd() * 60 | 0), k = 1 + (rnd() * 6 | 0);
        const a = Float32Array.from({ length: n }, () => rnd() < 0.15 ? NaN : rnd() * 100);
        const sel = new Set(simd.topkIndices(a, Math.min(k, n)));
        let minSel = Infinity, nonNaN = 0;
        for (const i of sel) if (!Number.isNaN(a[i])) minSel = Math.min(minSel, a[i]);
        for (let i = 0; i < n; i++) if (!Number.isNaN(a[i])) nonNaN++;
        let selNonNaN = 0; for (const i of sel) if (!Number.isNaN(a[i])) selNonNaN++;
        if (selNonNaN !== Math.min(sel.size, nonNaN)) bad++;
        for (let i = 0; i < n; i++) if (!sel.has(i) && !Number.isNaN(a[i]) && a[i] > minSel) bad++;
    }
    ok(bad === 0, "[red] M1c-03: no unselected non-NaN value exceeds a selected one, NaN ranks lowest (" + bad + " violations)");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
