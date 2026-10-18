// simd gather / f64Gather / scatterAdd / f64ScatterAdd against plain index loops (the definition), the
// validate-before-write rule (a bad index leaves the destination untouched), overlap and type refusals.
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
{
    const nanBits = new Uint32Array([0x7fc00001, 0x80000000, 1]), out = new Uint32Array(3);
    gather(out, nanBits, Int32Array.of(2, 0, 1));
    ok(out[0] === 1 && out[1] === 0x7fc00001 && out[2] === 0x80000000, "gather copies raw 32-bit patterns (NaN payload, -0) untouched");
}
{
    const dst = new Float32Array(4).fill(1), before = Array.from(dst);
    throwsT(() => scatterAdd(dst, Int32Array.of(0, 1, 4), Float32Array.of(1, 1, 1)), RangeError, "out-of-range index refused");
    ok(JSON.stringify(Array.from(dst)) === JSON.stringify(before), "a refused scatterAdd writes nothing (indices validated first)");
    throwsT(() => scatterAdd(dst, Int32Array.of(-1), Float32Array.of(1)), RangeError, "negative index refused");
    const out = new Float32Array(2);
    throwsT(() => gather(out, new Float32Array(3), Int32Array.of(0, 3)), RangeError, "gather index past src refused");
    ok(out[0] === 0, "a refused gather writes nothing");
    throwsT(() => gather(new Float32Array(3), new Float32Array(3), Int32Array.of(0, 1)), RangeError, "out/idx length mismatch refused");
    throwsT(() => scatterAdd(dst, Int32Array.of(0), Float32Array.of(1, 2)), RangeError, "idx/vals length mismatch refused");
    const buf = new Float32Array(10);
    throwsT(() => gather(buf.subarray(0, 4), buf.subarray(2, 8), Int32Array.of(0, 1, 2, 3)), RangeError, "overlapping out and src refused");
    throwsT(() => scatterAdd(buf.subarray(0, 4), Int32Array.of(0), buf.subarray(3, 4)), RangeError, "overlapping dst and vals refused");
    throwsT(() => scatterAdd(new Int32Array(4), Int32Array.of(0), new Int32Array(1)), TypeError, "Int32Array dst for scatterAdd refused");
    throwsT(() => gather(new Float32Array(1), new Float32Array(1), Uint32Array.of(0)), TypeError, "idx must be an Int32Array");
    throwsT(() => f64Gather(new Float32Array(1), new Float64Array(1), Int32Array.of(0)), TypeError, "f64Gather wants Float64Array out");
}
print("test_simd_gather_scatter: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
