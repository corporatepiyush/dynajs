// timeout: 300
// covers: dyna:simd.argmax
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
const __kitS = __kitSections("test_simd_argmax_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("[red] M1c-12: argmax/argmin skip a NaN at any of 34 positions", () => {
    const rnd = lcg(11);
    let bad = 0;
    for (let p = 0; p < 34; p++) {
        const a = Float32Array.from({ length: 34 }, () => f(rnd() * 200 - 100));
        a[p] = NaN;
        let hi = -1, lo = -1;
        for (let i = 0; i < 34; i++) {
            if (Number.isNaN(a[i])) continue;
            if (hi < 0 || a[i] > a[hi]) hi = i;
            if (lo < 0 || a[i] < a[lo]) lo = i;
        }
        if (simd.argmax(a) !== hi) bad++;
        if (simd.argmin(a) !== lo) bad++;
    }
    ok(bad === 0, "[red] M1c-12: argmax/argmin skip a NaN at any of 34 positions (" + bad + " of 68 wrong)");
    ok(simd.argmax(new Float32Array(9).fill(NaN)) === 0, "M1c-12: all-NaN argmax is 0");
    ok(Array.from(simd.cummax(Float32Array.of(5, NaN, 7, 1))).join() === "5,5,7,7", "[red SSE4.2] M1c-12: cummax carries over a NaN (" + Array.from(simd.cummax(Float32Array.of(5, NaN, 7, 1))) + ")");
    ok(simd.argmax(Float32Array.of(1, 9, 3, 9)) === 1, "control: argmax returns the first of equal maxima");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
