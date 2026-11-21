// timeout: 300
// covers: dyna:simd.sum
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
const __kitS = __kitSections("test_simd_sum_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("[red at 2e7] M1c-08: sum", () => {
    for (const c of [0.1, 1, 1 / 3]) {
        for (const n of [1e5, 1e6, 2e7]) {
            const a = new Float32Array(n).fill(c), cf = f(c), want = cf * n;
            const tol = 1e-6 * want;
            const s = simd.sum(a), m = simd.mean(a), l1 = simd.normL1(a), d = simd.dot(a, a);
            ok(Math.abs(s - want) <= tol, "[red at 2e7] M1c-08: sum(fill(" + c + ", " + n + ")) = " + s + ", want " + want);
            ok(Math.abs(m - cf) <= 1e-6 * cf, "M1c-08: mean(fill(" + c + ", " + n + ")) = " + m);
            ok(Math.abs(l1 - want) <= tol, "M1c-08: normL1(fill(" + c + ", " + n + ")) = " + l1);
            ok(Math.abs(d - cf * cf * n) <= 1e-6 * cf * cf * n, "M1c-08: dot(a,a) fill(" + c + ", " + n + ") = " + d);
        }
    }
    ok(Math.abs(simd.variance(new Float32Array(2e7).fill(1))) < 1e-9, "[red] M1c-08: variance of a constant array is 0");
    const rnd = lcg(3), a = Float32Array.from({ length: 1000 }, () => rnd() * 2 - 1);
    let ref = 0; for (const x of a) ref += x;
    ok(Math.abs(simd.sum(a) - ref) <= 1e-12 * 1000, "control: sum of 1000 mixed-sign values equals the double loop (" + simd.sum(a) + " vs " + ref + ")");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
