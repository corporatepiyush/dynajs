// covers: dyna:simd.i32Scale
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_scale_int");
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
    for (const s of [7, -3, 65537, 0, -2147483648]) {
        const a = geni(len, 5), c = Int32Array.from(a);
        const r = i32Scale(c, s);
        assert(r === c, "i32Scale returns its array, len=" + len);
        for (let i = 0; i < len; i++) assert(c[i] === Math.imul(a[i], s), "i32Scale[" + i + "] len=" + len + " s=" + s);
    }
}

__kitS.section("i32Scale coerces scalar first", () => {
    const a = Int32Array.of(1, 2, 3, 4, 5);
    let calls = 0;
    i32Scale(a, { valueOf() { calls++; return 3; } });
    assert(calls === 1 && a[0] === 3 && a[4] === 15, "i32Scale coerces scalar first");
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
