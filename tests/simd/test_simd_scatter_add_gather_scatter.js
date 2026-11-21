// covers: dyna:simd.scatterAdd
// simd gather / f64Gather / scatterAdd / f64ScatterAdd against plain index loops (the definition), the
// validate-before-write rule (a bad index leaves the destination untouched), overlap and type refusals.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_scatter_add_gather_scatter");
import { gather, f64Gather, scatterAdd, f64ScatterAdd } from "dyna:simd";
import { Random } from "dyna:random";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const throwsT = (f, cls, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof cls, m + " threw " + e.name + ": " + e.message); } };
const rng = new Random(5);

__kitS.section("out-of-range index refused", () => {
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
});
__kitS.end();
print("test_simd_gather_scatter: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
