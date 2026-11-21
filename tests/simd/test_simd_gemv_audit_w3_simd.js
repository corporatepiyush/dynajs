// timeout: 300
// covers: dyna:simd.gemv
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
const __kitS = __kitSections("test_simd_gemv_audit_w3_simd");
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------

__kitS.section("control: gemv with a separate output", () => {
    const P = Float32Array.of(0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    const x = Float32Array.of(1, 2, 3, 4), y = new Float32Array(4);
    ok(Array.from(simd.gemv(y, P, x, 4, 4, 0)).join() === "2,3,4,1", "control: gemv with a separate output");
    let err = "none";
    try { simd.gemv(x, P, x, 4, 4, 0); } catch (e) { err = e.constructor.name; }
    ok(err === "TypeError", "[red] M1c-16: gemv with y aliasing x is refused, not silently wrong (" + err + ")");
    const buf = new Float32Array(20);
    err = "none";
    try { simd.gemv(buf.subarray(0, 4), buf.subarray(2, 18), x, 4, 4, 0); } catch (e) { err = e.constructor.name; }
    ok(err === "TypeError", "[red] M1c-16: gemv with y partially overlapping a is refused (" + err + ")");
});
__kitS.end();

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
