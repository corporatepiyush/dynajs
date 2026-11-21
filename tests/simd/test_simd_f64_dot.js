// covers: dyna:simd.f64Dot
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_f64_dot");
import { f64Sum, f64Dot, f64Max, f64Min, f64Scale, f64Axpy } from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1000];
const rnd64 = (len, seed) => Float64Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 17) - 8.5);

__kitS.section("f64Dot empty = 0", () => {
assert(f64Dot(new Float64Array(0), new Float64Array(0)) === 0, "f64Dot empty = 0");
});
__kitS.end();

print("test_simd_f64: all tests passed (" + n + " assertions)");
