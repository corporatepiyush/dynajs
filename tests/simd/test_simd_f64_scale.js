// covers: dyna:simd.f64Scale
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_f64_scale");
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

for (const len of SIZES) {
    for (const s of [2.5, -3, 0.1, 0]) {
        const a = rnd64(len, 4), c = Float64Array.from(a);
        const r = f64Scale(c, s);
        assert(r === c, "f64Scale returns its array, len=" + len);
        for (let i = 0; i < len; i++) assert(c[i] === a[i] * s, "f64Scale[" + i + "] len=" + len + " s=" + s);
    }
}

__kitS.section("f64Scale coerces scalar first", () => {
    const a = new Float64Array([1, 2, 3, 4]);
    let calls = 0;
    f64Scale(a, { valueOf() { calls++; return 2; } });
    assert(calls === 1 && a[0] === 2 && a[3] === 8, "f64Scale coerces scalar first");

    const y = new Float64Array([1, 2, 3, 4]), x = new Float64Array([10, 20, 30, 40]);
    let c2 = 0;
    f64Axpy(y, { valueOf() { c2++; return 3; } }, x);
    assert(c2 === 1 && y[0] === 31 && y[3] === 124, "f64Axpy coerces scalar first");
});
__kitS.end();

print("test_simd_f64: all tests passed (" + n + " assertions)");
