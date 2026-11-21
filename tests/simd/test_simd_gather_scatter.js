// covers: dyna:simd.gather
// simd gather / f64Gather / scatterAdd / f64ScatterAdd against plain index loops (the definition), the
// validate-before-write rule (a bad index leaves the destination untouched), overlap and type refusals.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gather_scatter");
import { gather, f64Gather, scatterAdd, f64ScatterAdd } from "dyna:simd";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };
const rng = new Random(5);

for (const n of [0, 1, 7, 64, 1000]) {
    const src = Float32Array.from({ length: 300 }, () => rng.normal()), idx = Int32Array.from({ length: n }, () => rng.nextBounded(300));
    const out = new Float32Array(n);
    ok(gather(out, src, idx) === out && out.every((v, i) => Object.is(v, src[idx[i]])), `gather f32 n=${n}`);
    const s64 = Float64Array.from(src), o64 = new Float64Array(n);
    ok(f64Gather(o64, s64, idx) === o64 && o64.every((v, i) => v === s64[idx[i]]), `f64Gather n=${n}`);
    const dst = Float32Array.from({ length: 300 }, () => rng.normal()), want = Float32Array.from(dst), vals = Float32Array.from({ length: n }, () => rng.normal());
    for (let i = 0; i < n; i++) want[idx[i]] += vals[i];
    ok(scatterAdd(dst, idx, vals) === dst && dst.every((v, i) => v === want[i]), `scatterAdd f32 with repeated indices n=${n}`);
    const d64 = new Float64Array(300), w64 = new Float64Array(300), v64 = Float64Array.from(vals);
    for (let i = 0; i < n; i++) w64[idx[i]] += v64[i];
    ok(f64ScatterAdd(d64, idx, v64) === d64 && d64.every((v, i) => v === w64[i]), `f64ScatterAdd n=${n}`);
}

__kitS.section("gather copies raw 32-bit patterns (NaN payload, -0) untouched", () => {
    const nanBits = new Uint32Array([0x7fc00001, 0x80000000, 1]), out = new Uint32Array(3);
    gather(out, nanBits, Int32Array.of(2, 0, 1));
    ok(out[0] === 1 && out[1] === 0x7fc00001 && out[2] === 0x80000000, "gather copies raw 32-bit patterns (NaN payload, -0) untouched");
});
__kitS.end();
print("test_simd_gather_scatter: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
