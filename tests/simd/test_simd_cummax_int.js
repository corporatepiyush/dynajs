// covers: dyna:simd.cummax
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_cummax_int");
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

for (const len of SIZES) {
    const ai = geni(len, 7), ci = Int32Array.from(ai);
    cummax(ci);
    let mI = -0x80000000;
    for (let i = 0; i < len; i++) { if (ai[i] > mI) mI = ai[i]; assert(ci[i] === mI, "i32 cummax[" + i + "] len=" + len); }

    const af = Float32Array.from({ length: len }, (_, i) => ((i * 13 + 5) % 977) / 7 - 70);
    const cf = Float32Array.from(af);
    cummax(cf);
    let mF = -Infinity;
    for (let i = 0; i < len; i++) { if (af[i] > mF) mF = af[i]; assert(cf[i] === mF, "f32 cummax[" + i + "] len=" + len); }
}

__kitS.section("i32Add overflow wraps", () => {
    const o = new Int32Array(1);
    i32Add(o, Int32Array.of(2147483647), Int32Array.of(1));
    assert(o[0] === -2147483648, "i32Add overflow wraps");
    i32Mul(o, Int32Array.of(2147483647), Int32Array.of(2));
    assert(o[0] === -2, "i32Mul low32");
    const cw = Int32Array.of(2147483647, 1, 2147483647, 2);
    cumsum(cw);
    assert(cw[0] === 2147483647 && cw[1] === -2147483648, "cumsum overflow wraps");
    const km = Int32Array.of(-10, -3, -8, 5, 2, 5);
    cummax(km);
    assert(km[0] === -10 && km[3] === 5 && km[5] === 5, "i32 cummax dip");
    const fm = Float32Array.of(-3, -5, -1, -2);
    cummax(fm);
    assert(fm[0] === -3 && fm[1] === -3 && fm[2] === -1 && fm[3] === -1, "f32 cummax negatives (identity -inf)");
});

__kitS.section("f32 cummax NaN carry", () => {
    const v = Float32Array.from([1, NaN, 2, 3, 4, 5, 6, 7]);
    cummax(v);
    const want = [1, 1, 2, 3, 4, 5, 6, 7];
    for (let i = 0; i < 8; i++)
        assert(v[i] === want[i] || (Number.isNaN(want[i]) && Number.isNaN(v[i])),
               "f32 cummax NaN carry [" + i + "]");
    const w = Int32Array.of(2147483647, 2147483647, 2147483647);
    cumsum(w);
    assert(w[1] === -2 && w[2] === 2147483645, "i32 cumsum multi-wrap exact");
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
