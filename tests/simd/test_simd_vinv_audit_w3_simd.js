// timeout: 300
// covers: dyna:simd.vinv
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
const __kitS = __kitSections("test_simd_vinv_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("[red NEON] M1c-10: vinv of a positive subnormal is +Infinity", () => {
    ok(simd.vinv(Float32Array.of(1e-40, 1, 1, 1))[0] === Infinity, "[red NEON] M1c-10: vinv of a positive subnormal is +Infinity");
    let bad = 0, total = 0;
    for (let e = -140; e <= 120; e += 3) {
        for (const mant of [1, 1.3, 1.9]) {
            const x = f(mant * Math.pow(2, e));
            if (x === 0 || !Number.isFinite(x)) continue;
            for (let n = 1; n <= 9; n++) {
                const sq = simd.vsqrt(new Float32Array(n).fill(x)), iv = simd.vinv(new Float32Array(n).fill(x)), rs = simd.vrsqrt(new Float32Array(n).fill(x));
                const wsq = f(Math.sqrt(x)), wiv = f(1 / x), wrs = f(1 / f(Math.sqrt(x)));
                for (let i = 0; i < n; i++) {
                    total += 3;
                    if (sq[i] !== wsq) bad++;
                    if (iv[i] !== wiv) bad++;
                    if (rs[i] !== wrs) bad++;
                }
            }
        }
    }
    ok(bad === 0, "[red NEON, SSE4.2] M1c-10: vsqrt/vinv/vrsqrt are correctly rounded and identical in body and tail (" + bad + " of " + total + " differ)");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
