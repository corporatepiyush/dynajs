// covers: dyna:simd.i32Add
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_add_int");
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

__kitS.section("i32Add/i32Mul return out, len", () => {
for (const len of SIZES) {
    const a = geni(len, 3), b = geni(len, 4);
    const so = new Int32Array(len), po = new Int32Array(len);
    const rs = i32Add(so, a, b), rp = i32Mul(po, a, b);
    assert(rs === so && rp === po, "i32Add/i32Mul return out, len=" + len);
    for (let i = 0; i < len; i++) {
        assert(so[i] === ((a[i] + b[i]) | 0), "i32Add[" + i + "] len=" + len);
        assert(po[i] === Math.imul(a[i], b[i]), "i32Mul[" + i + "] len=" + len);
    }
}
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
