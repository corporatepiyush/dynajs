// covers: dyna:simd.f64Axpy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_f64_axpy");
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

__kitS.section("f64Axpy returns y, len", () => {
for (const len of SIZES) {
    for (const al of [2.5, -3, 0.1]) {
        const y0 = rnd64(len, 5), x = rnd64(len, 6), y = Float64Array.from(y0);
        const r = f64Axpy(y, al, x);
        assert(r === y, "f64Axpy returns y, len=" + len);
        for (let i = 0; i < len; i++) assert(y[i] === y0[i] + al * x[i], "f64Axpy[" + i + "] len=" + len + " a=" + al);
    }
}
});
__kitS.end();

print("test_simd_f64: all tests passed (" + n + " assertions)");
