// covers: dyna:simd.cumsum
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_cumsum_int");
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

__kitS.section("cumsum returns its array (i32), len", () => {
for (const len of SIZES) {
    const ai = geni(len, 6), ci = Int32Array.from(ai);
    const ri = cumsum(ci);
    assert(ri === ci, "cumsum returns its array (i32), len=" + len);
    let accI = 0;
    for (let i = 0; i < len; i++) { accI = (accI + ai[i]) | 0; assert(ci[i] === accI, "i32 cumsum[" + i + "] len=" + len); }

    const af = genf(len, 6), cf = Float32Array.from(af);
    const rf = cumsum(cf);
    assert(rf === cf, "cumsum returns its array (f32), len=" + len);
    let accF = 0;
    for (let i = 0; i < len; i++) { accF += af[i]; assert(cf[i] === accF, "f32 cumsum[" + i + "] len=" + len); }
}
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
