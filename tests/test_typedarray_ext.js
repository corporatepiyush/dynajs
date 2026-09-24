/* test_typedarray_ext.js — SIMD-accelerated TypedArray reductions.
 * Float64/Float32/Int32 use the multi-ISA SIMD kernels; other numeric element
 * types use a scalar loop. Run: dynajs tests/test_typedarray_ext.js
 * (Core builtins — present in every build; verify on macOS AND Linux amd64/arm64.) */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(Object.is(a, b) || a === b, m + " (got " + a + ", want " + b + ")"); }
/* SIMD sum reorders additions -> not bit-identical; compare with a rel tolerance */
function near(a, b, m, tol = 1e-9) { assert(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b)), m + " (got " + a + ", want ≈" + b + ")"); }

/* ---- Float64Array (SIMD f64_sum/f64_min/f64_max) ---- */
{
    const a = new Float64Array([3.5, -1.5, 4.5, 1.0, 5.5]);
    near(a.sum(), 13.0, "f64 _sum");
    eq(a.min(), -1.5, "f64 _min (bit-exact)");
    eq(a.max(), 5.5, "f64 _max (bit-exact)");
    near(a.mean(), 2.6, "f64 _mean");
    near(a.average(), 2.6, "f64 _average alias");
}
/* ---- Int32Array (SIMD i32_sum widening / i32_min / i32_max) ---- */
{
    const a = new Int32Array([3, 1, 4, 1, 5, 9, 2, 6, -7]);
    eq(a.sum(), 24, "i32 _sum exact (int64 widening)");
    eq(a.min(), -7, "i32 _min exact");
    eq(a.max(), 9, "i32 _max exact");
    near(a.mean(), 24 / 9, "i32 _mean");
}
/* ---- Float32Array (SIMD f32 sum/min/max) ---- */
{
    const a = new Float32Array([1, 2, 3, 4, 5]);
    near(a.sum(), 15, "f32 _sum", 1e-5);
    eq(a.max(), 5, "f32 _max");
    eq(a.min(), 1, "f32 _min");
}
/* ---- scalar element types: Uint8/Int8/Int16/Uint16/Uint32/Float16 ---- */
eq(new Uint8Array([10, 20, 30, 250]).sum(), 310, "u8 _sum");
eq(new Uint8Array([10, 20, 30]).max(), 30, "u8 _max");
eq(new Int8Array([-5, 5, -10, 10]).min(), -10, "i8 _min");
eq(new Int16Array([100, -200, 300]).sum(), 200, "i16 _sum");
eq(new Uint16Array([1000, 2000]).max(), 2000, "u16 _max");
eq(new Uint32Array([1, 2, 3000000000]).sum(), 3000000003, "u32 _sum");
near(new Float16Array([1.5, 2.5, 3.0]).sum(), 7.0, "f16 _sum", 1e-2);

/* ---- large-array SIMD correctness (exercises the vector tail + remainder) ---- */
{
    const N = 10000, a = new Float64Array(N);
    let expect = 0;
    for (let i = 0; i < N; i++) { a[i] = (i % 7) - 3 + i * 0.001; expect += a[i]; }
    near(a.sum(), expect, "f64 _sum large (SIMD reorder within tolerance)", 1e-9);
    let mn = Infinity, mx = -Infinity;
    for (let i = 0; i < N; i++) { if (a[i] < mn) mn = a[i]; if (a[i] > mx) mx = a[i]; }
    eq(a.min(), mn, "f64 _min large (bit-exact)");
    eq(a.max(), mx, "f64 _max large (bit-exact)");
    const b = new Int32Array(N);
    let is = 0n; for (let i = 0; i < N; i++) { b[i] = (i * 131071) | 0; is += BigInt(b[i]); }
    eq(BigInt(b.sum()), is, "i32 _sum large exact");
}

/* ---- _dot (SIMD dot product) ---- */
near(new Float64Array([1, 2, 3]).dot(new Float64Array([4, 5, 6])), 32, "f64 _dot");
eq(new Int32Array([1, 2, 3]).dot(new Int32Array([4, 5, 6])), 32, "i32 _dot exact");
near(new Float32Array([1, 2]).dot(new Float32Array([3, 4])), 11, "f32 _dot", 1e-5);
eq(new Uint8Array([1, 2, 3]).dot(new Uint8Array([1, 1, 1])), 6, "u8 _dot (scalar)");
eq(new Float64Array([]).dot(new Float64Array([])), 0, "dot empty → 0");
{ let t = false; try { new Float64Array([1, 2]).dot(new Float64Array([1])); } catch { t = true; } assert(t, "dot length mismatch → RangeError"); }
{ let t = false; try { new Float64Array([1]).dot(new Int32Array([1])); } catch { t = true; } assert(t, "dot type mismatch → TypeError"); }
{ let t = false; try { new Float64Array([1]).dot([1]); } catch { t = true; } assert(t, "dot non-TypedArray → TypeError"); }
{ const N = 1000; const a = new Float64Array(N), b = new Float64Array(N); let e = 0; for (let i = 0; i < N; i++) { a[i] = i * 0.5; b[i] = (i % 3) - 1; e += a[i] * b[i]; } near(a.dot(b), e, "f64 _dot large (SIMD)", 1e-6); }

/* ---- edge cases ---- */
eq(new Float64Array([]).sum(), 0, "empty _sum → 0");
eq(new Float64Array([]).min(), undefined, "empty _min → undefined");
eq(new Float64Array([]).max(), undefined, "empty _max → undefined");
assert(Number.isNaN(new Float64Array([]).mean()), "empty _mean → NaN");
{ let t = false; try { new BigInt64Array([1n, 2n]).sum(); } catch { t = true; } assert(t, "BigInt64Array _sum throws"); }

/* ---- demarcation: non-enumerable, on %TypedArray%.prototype ---- */
assert(!Object.keys(Float64Array.prototype).includes("sum"), "sum non-enumerable");
const TAproto = Object.getPrototypeOf(Float64Array.prototype);
assert(Object.getOwnPropertyNames(TAproto).includes("sum"), "sum lives on %TypedArray%.prototype (shared)");
assert(new Int8Array([1]).sum !== undefined, "inherited by every typed array");

print("test_typedarray_ext: all tests passed (" + n + " assertions)");
