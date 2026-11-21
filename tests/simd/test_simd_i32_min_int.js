// covers: dyna:simd.i32Min
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_min_int");
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

__kitS.section("i32Sum len", () => {
for (const len of SIZES) {
    const a = geni(len, 1), b = geni(len, 2);
    let s = 0, d = 0, mn = 0x7fffffff, mx = -0x80000000;
    for (let i = 0; i < len; i++) {
        s += a[i]; d += a[i] * b[i];
        if (a[i] < mn) mn = a[i];
        if (a[i] > mx) mx = a[i];
    }
    assert(i32Sum(a) === s, "i32Sum len=" + len);
    assert(i32Dot(a, b) === d, "i32Dot len=" + len);
    if (len === 0) {
        for (const [name, fn] of [["i32Min", i32Min], ["i32Max", i32Max]]) {
            let t = false; try { fn(a); } catch { t = true; }
            assert(t, name + " empty throws");
        }
    } else {
        assert(i32Min(a) === mn, "i32Min len=" + len);
        assert(i32Max(a) === mx, "i32Max len=" + len);
    }
}
});
__kitS.end();

function throws(fn) { try { fn(); return false; } catch { return true; } }

print("test_simd_int: all tests passed (" + n + " assertions)");
