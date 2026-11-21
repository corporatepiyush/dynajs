// covers: dyna:simd.f64Max
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_f64_max");
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

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

__kitS.section("f64Max NaN poisons", () => {
    /* CONTRACT CHANGE (FIX-4): the module doc pins "NaN in the input propagates to
       NaN through every reduction" -- f64Max/f64Min previously skipped NaN. */
    assert(Number.isNaN(f64Max(Float64Array.of(1, NaN, 3))), "f64Max NaN poisons");
    assert(Number.isNaN(f64Min(Float64Array.of(1, NaN, 3))), "f64Min NaN poisons");
    assert(Number.isNaN(f64Max(Float64Array.of(NaN, NaN))), "f64Max all-NaN = NaN");
    assert(Number.isNaN(f64Min(Float64Array.of(NaN, NaN))), "f64Min all-NaN = NaN");
    const y = Float64Array.of(NaN, 1);
    f64Axpy(y, 0, Float64Array.of(2, 2));
    assert(Number.isNaN(y[0]) && y[1] === 1, "f64Axpy elementwise exact");
});
__kitS.end();

print("test_simd_f64: all tests passed (" + n + " assertions)");
