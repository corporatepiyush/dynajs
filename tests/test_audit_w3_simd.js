// timeout: 300
// tests/test_audit_w3_simd.js -- audit wave 3, dyna:simd value correctness.
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
import * as simd from "dyna:simd";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; if (failures <= 40) print("  FAIL:", msg); } }
const f = Math.fround;
function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296; }

// ---- M1c-01 -------------------------------------------------------------
{
    let bad = 0;
    for (let n = 1; n <= 130; n++) {
        if (simd.max(new Float32Array(n).fill(-Infinity)) !== -Infinity) bad++;
        if (simd.min(new Float32Array(n).fill(Infinity)) !== Infinity) bad++;
        if (simd.f64Max(new Float64Array(n).fill(-Infinity)) !== -Infinity) bad++;
        if (simd.f64Min(new Float64Array(n).fill(Infinity)) !== Infinity) bad++;
    }
    ok(bad === 0, "[red NEON/scalar] M1c-01: min/max of an all-infinite array is that infinity (" + bad + " of 520 wrong)");
    const a = Float32Array.from({ length: 77 }, (_, i) => (i * 37 % 101) - 50);
    ok(simd.max(a) === Math.max(...a) && simd.min(a) === Math.min(...a), "control: finite min/max");
}

// ---- M1c-02 -------------------------------------------------------------
{
    let bad = 0, total = 0;
    for (let n = 1; n <= 130; n += 3) {
        for (let p = 0; p < n; p++) {
            const a = new Float32Array(n).fill(1), b = new Float32Array(n);
            a[p] = NaN; total++;
            if (!Number.isNaN(simd.distCheb(a, b))) bad++;
        }
    }
    ok(bad === 0, "[red] M1c-02: distCheb with a NaN at any index is NaN (" + bad + " of " + total + " wrong)");
    ok(simd.distCheb(Float32Array.of(1, -7, 3), Float32Array.of(0, 0, 0)) === 7, "control: distCheb of finite data");
}

// ---- M1c-03 -------------------------------------------------------------
{
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
}

// ---- M1c-04 -------------------------------------------------------------
{
    let bad = 0;
    for (let n = 1; n <= 130; n++) {
        for (const c of [1, -2.5, 0.3]) {
            const a = simd.silu(new Float32Array(n).fill(c));
            for (let i = 1; i < n; i++) if (a[i] !== a[0]) { bad++; break; }
        }
    }
    ok(bad === 0, "[red] M1c-04: silu of a constant array is one value at every length (" + bad + " of 390 arrays mixed two answers)");
}

// ---- M1c-08 -------------------------------------------------------------
{
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
}

// ---- M1c-09 -------------------------------------------------------------
// silu uses the documented fast exponential (up to ~6% off mid-range), so the
// tolerance is 7%: this section pins the LIMITS -- silu(-100) is 0 not -100,
// vexp(89.5) is +Infinity not 0 -- which a 7% band cannot confuse.
{
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
}

// ---- M1c-10 -------------------------------------------------------------
{
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
}

// ---- M1c-12 -------------------------------------------------------------
// Rule (now in the d.ts): argmax/argmin ignore NaN and return the first
// extreme among the rest (0 if every element is NaN); cummax carries the
// running maximum over NaN positions.
{
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
}

// ---- M1c-16 -------------------------------------------------------------
{
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
}

ok(simd.logSoftmax(Float32Array.of(-9.781))[0] === 0, "control: logSoftmax of one element is 0");

print("test_audit_w3_simd: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_simd: " + failures + " failures");
