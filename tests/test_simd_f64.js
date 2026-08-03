/* test_simd_f64.js — dyna:simd double-precision (f64) kernels over
 * Float64Array. Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_simd_f64.js
 *
 * JS Number IS f64, so the JS reference below computes the SAME IEEE-754 ops as
 * the kernels:
 *   - f64Scale / f64Axpy are BIT-EXACT vs JS (===). axpy is a non-fused
 *     multiply-then-add on every ISA, matching JS `y[i] + a*x[i]` (JS has no
 *     fused multiply-add), so === holds.
 *   - f64Max / f64Min are exact (===).
 *   - f64Sum / f64Dot REORDER additions, so they are checked with a
 *     reorder-aware (condition-number) tolerance: |got-ref| <= 1e-12 * Σ|terms|.
 */
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

/* sizes hit every f64 vector width (NEON/SSE2=2, AVX=4, AVX-512=8) plus
 * multi-vector bodies and odd tails. */
const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

/* ---------- f64Sum / f64Dot: reorder-aware tolerance ---------- */
for (const len of SIZES) {
    const a = rnd64(len, 1), b = rnd64(len, 2);
    let s = 0, sCond = 0, d = 0, dCond = 0;
    for (let i = 0; i < len; i++) {
        s += a[i]; sCond += Math.abs(a[i]);
        d += a[i] * b[i]; dCond += Math.abs(a[i] * b[i]);
    }
    assert(Math.abs(f64Sum(a) - s) <= 1e-12 * Math.max(1, sCond), "f64Sum len=" + len);
    assert(Math.abs(f64Dot(a, b) - d) <= 1e-12 * Math.max(1, dCond), "f64Dot len=" + len);
}
assert(f64Sum(new Float64Array(0)) === 0, "f64Sum empty = 0");
assert(f64Dot(new Float64Array(0), new Float64Array(0)) === 0, "f64Dot empty = 0");

/* ---------- f64Max / f64Min: exact; empty throws ---------- */
for (const len of SIZES) {
    const a = rnd64(len, 3);
    if (len === 0) {
        for (const [name, fn] of [["f64Max", f64Max], ["f64Min", f64Min]]) {
            let t = false; try { fn(a); } catch { t = true; }
            assert(t, name + ": empty array throws");
        }
        continue;
    }
    let mx = a[0], mn = a[0];
    for (let i = 1; i < len; i++) { if (a[i] > mx) mx = a[i]; if (a[i] < mn) mn = a[i]; }
    assert(f64Max(a) === mx, "f64Max len=" + len);
    assert(f64Min(a) === mn, "f64Min len=" + len);
}

/* ---------- f64Scale: in-place, bit-exact, returns the same array ---------- */
for (const len of SIZES) {
    for (const s of [2.5, -3, 0.1, 0]) {
        const a = rnd64(len, 4), c = Float64Array.from(a);
        const r = f64Scale(c, s);
        assert(r === c, "f64Scale returns its array, len=" + len);
        for (let i = 0; i < len; i++) assert(c[i] === a[i] * s, "f64Scale[" + i + "] len=" + len + " s=" + s);
    }
}

/* ---------- f64Axpy: in-place non-fused, bit-exact, returns y ---------- */
for (const len of SIZES) {
    for (const al of [2.5, -3, 0.1]) {
        const y0 = rnd64(len, 5), x = rnd64(len, 6), y = Float64Array.from(y0);
        const r = f64Axpy(y, al, x);
        assert(r === y, "f64Axpy returns y, len=" + len);
        for (let i = 0; i < len; i++) assert(y[i] === y0[i] + al * x[i], "f64Axpy[" + i + "] len=" + len + " a=" + al);
    }
}

/* ---------- type + shape errors ---------- */
{
    let t = false; try { f64Dot(new Float64Array(3), new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Dot length mismatch throws");
    t = false; try { f64Axpy(new Float64Array(3), 1, new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Axpy length mismatch throws");
    t = false; try { f64Sum(new Float32Array(4)); } catch { t = true; } /* bpe 4 != 8 */
    assert(t, "f64Sum on a Float32Array throws");
    t = false; try { f64Sum(new Uint8Array(8)); } catch { t = true; }
    assert(t, "f64Sum on a Uint8Array throws");
    t = false; try { f64Sum([1, 2, 3]); } catch { t = true; }
    assert(t, "f64Sum on a plain array throws");
}

/* ---------- reentrancy: a scalar arg's valueOf runs BEFORE the buffer is
 * resolved (coerce-first discipline), so it can't corrupt the operation. ---- */
{
    const a = new Float64Array([1, 2, 3, 4]);
    let calls = 0;
    f64Scale(a, { valueOf() { calls++; return 2; } });
    assert(calls === 1 && a[0] === 2 && a[3] === 8, "f64Scale coerces scalar first");

    const y = new Float64Array([1, 2, 3, 4]), x = new Float64Array([10, 20, 30, 40]);
    let c2 = 0;
    f64Axpy(y, { valueOf() { c2++; return 3; } }, x);
    assert(c2 === 1 && y[0] === 31 && y[3] === 124, "f64Axpy coerces scalar first");
}

print("test_simd_f64: all tests passed (" + n + " assertions)");
