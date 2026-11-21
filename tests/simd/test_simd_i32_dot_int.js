// covers: dyna:simd.i32Dot
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_dot_int");
import {
    i32Sum, i32Min, i32Max, i32Dot, i32Add, i32Mul, i32Scale, cumsum, cummax,
} from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 33, 100, 1000];

const geni = (len, seed) =>
    Int32Array.from({ length: len }, (_, i) => (((i * 2654435761 + seed * 40503) >>> 0) % 2000001) - 1000000);
const genf = (len, seed) =>
    Float32Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 2001) - 1000);

__kitS.section("i32Dot empty = 0", () => {
assert(i32Dot(new Int32Array(0), new Int32Array(0)) === 0, "i32Dot empty = 0");
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
