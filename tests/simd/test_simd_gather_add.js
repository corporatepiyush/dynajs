// covers: integration dyna:simd.gatherAdd dyna:simd.f64GatherAdd -- the float32 and float64 forms are one definition and one kernel, checked against one loop
// gatherAdd / f64GatherAdd (dst[i] += src[idx[i]]) against the plain index loop (the definition,
// float32 rounding via Math.fround), repeated and out-of-order indices, the validate-before-write
// rule (a bad index leaves dst untouched), length, type and overlap refusals.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gather_add");
import { gatherAdd, f64GatherAdd } from "dyna:simd";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };
const rng = new Random(7);

__kitS.section("matches the index loop", () => {
    for (const n of [0, 1, 7, 64, 1000]) {
        const src = Float32Array.from({ length: 300 }, () => rng.normal()), idx = Int32Array.from({ length: n }, () => rng.nextBounded(300));
        const dst = Float32Array.from({ length: n }, () => rng.normal()), want = Float32Array.from(dst);
        for (let i = 0; i < n; i++) want[i] = Math.fround(want[i] + src[idx[i]]);
        ok(gatherAdd(dst, src, idx) === dst && dst.every((v, i) => Object.is(v, want[i])), `gatherAdd f32 n=${n}`);
        const s64 = Float64Array.from({ length: 300 }, () => rng.normal()), d64 = Float64Array.from({ length: n }, () => rng.normal()), w64 = Float64Array.from(d64);
        for (let i = 0; i < n; i++) w64[i] += s64[idx[i]];
        ok(f64GatherAdd(d64, s64, idx) === d64 && d64.every((v, i) => v === w64[i]), `f64GatherAdd n=${n}`);
    }
    const d = Float32Array.of(1, 1, 1), s = Float32Array.of(10, 20);
    gatherAdd(d, s, Int32Array.of(1, 1, 0));
    ok(d[0] === 21 && d[1] === 21 && d[2] === 11, "repeated index reads the same source twice");
});

__kitS.section("refusals leave dst untouched", () => {
    const d = Float32Array.of(1, 2, 3), s = Float32Array.of(5, 6);
    throwsT(() => gatherAdd(d, s, Int32Array.of(0, 2, 1)), RangeError, "index past src");
    throwsT(() => gatherAdd(d, s, Int32Array.of(0, -1, 1)), RangeError, "negative index");
    ok(d[0] === 1 && d[1] === 2 && d[2] === 3, "bad index: no element written");
    throwsT(() => gatherAdd(d, s, Int32Array.of(0, 1)), RangeError, "idx shorter than dst");
    throwsT(() => gatherAdd(new Int32Array(3), s, Int32Array.of(0, 1, 0)), TypeError, "Int32Array dst");
    throwsT(() => gatherAdd(d, new Uint32Array(2), Int32Array.of(0, 1, 0)), TypeError, "Uint32Array src");
    throwsT(() => gatherAdd(d, s, Float32Array.of(0, 1, 0)), TypeError, "Float32Array idx");
    throwsT(() => f64GatherAdd(d, s, Int32Array.of(0, 1, 0)), TypeError, "f64GatherAdd on Float32Array");
    const buf = new Float32Array(8);
    throwsT(() => gatherAdd(buf.subarray(0, 4), buf.subarray(2, 8), Int32Array.of(0, 1, 2, 3)), RangeError, "dst overlaps src");
    const ib = new ArrayBuffer(32);
    throwsT(() => gatherAdd(new Float32Array(ib, 0, 4), Float32Array.of(1), new Int32Array(ib, 8, 4)), RangeError, "dst overlaps idx");
});

__kitS.end();
print("test_simd_gather_add: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
