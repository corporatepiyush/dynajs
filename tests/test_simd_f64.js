import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

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

for (const len of SIZES) {
    for (const s of [2.5, -3, 0.1, 0]) {
        const a = rnd64(len, 4), c = Float64Array.from(a);
        const r = f64Scale(c, s);
        assert(r === c, "f64Scale returns its array, len=" + len);
        for (let i = 0; i < len; i++) assert(c[i] === a[i] * s, "f64Scale[" + i + "] len=" + len + " s=" + s);
    }
}

for (const len of SIZES) {
    for (const al of [2.5, -3, 0.1]) {
        const y0 = rnd64(len, 5), x = rnd64(len, 6), y = Float64Array.from(y0);
        const r = f64Axpy(y, al, x);
        assert(r === y, "f64Axpy returns y, len=" + len);
        for (let i = 0; i < len; i++) assert(y[i] === y0[i] + al * x[i], "f64Axpy[" + i + "] len=" + len + " a=" + al);
    }
}

{
    let t = false; try { f64Dot(new Float64Array(3), new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Dot length mismatch throws");
    t = false; try { f64Axpy(new Float64Array(3), 1, new Float64Array(4)); } catch { t = true; }
    assert(t, "f64Axpy length mismatch throws");
    t = false; try { f64Sum(new Float32Array(4)); } catch { t = true; }
    assert(t, "f64Sum on a Float32Array throws");
    t = false; try { f64Sum(new Uint8Array(8)); } catch { t = true; }
    assert(t, "f64Sum on a Uint8Array throws");
    t = false; try { f64Sum([1, 2, 3]); } catch { t = true; }
    assert(t, "f64Sum on a plain array throws");
}

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

{
    /* CONTRACT CHANGE (FIX-4): the module doc pins "NaN in the input propagates to
       NaN through every reduction" -- f64Max/f64Min previously skipped NaN. */
    assert(Number.isNaN(f64Max(Float64Array.of(1, NaN, 3))), "f64Max NaN poisons");
    assert(Number.isNaN(f64Min(Float64Array.of(1, NaN, 3))), "f64Min NaN poisons");
    assert(Number.isNaN(f64Max(Float64Array.of(NaN, NaN))), "f64Max all-NaN = NaN");
    assert(Number.isNaN(f64Min(Float64Array.of(NaN, NaN))), "f64Min all-NaN = NaN");
    const y = Float64Array.of(NaN, 1);
    f64Axpy(y, 0, Float64Array.of(2, 2));
    assert(Number.isNaN(y[0]) && y[1] === 1, "f64Axpy elementwise exact");
}

print("test_simd_f64: all tests passed (" + n + " assertions)");
