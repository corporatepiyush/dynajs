// covers: dyna:simd.i32Mean
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_mean_stats");
import {
    mean, variance, normalize, f64Mean, f64Variance, i32Mean, gemv,
} from "dyna:simd";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
const near = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-4);

let seed = 0x9e3779b9 | 0;
function rnd() {
    seed ^= seed << 13; seed |= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5; seed |= 0;
    return ((seed >>> 0) % 1000000) / 1000000 - 0.5;
}

function oracleMean(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s / a.length; }
function oracleVar(a) {
    const m = oracleMean(a);
    let v = 0;
    for (let i = 0; i < a.length; i++) v += (a[i] - m) * (a[i] - m);
    return v / a.length;
}

const MAGS = [1e-3, 1, 100, 1e4];

function randF32(len) {
    return Float32Array.from({ length: len }, (_, i) =>
        Math.fround(rnd() * 2 * MAGS[i % MAGS.length]));
}
function randF64(len) {
    return Float64Array.from({ length: len }, (_, i) => rnd() * 2 * MAGS[i % MAGS.length]);
}

const SIZES = [1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
               127, 128, 129, 255, 256, 257, 1000, 4096];

__kitS.section("i32Mean exact len", () => {
    for (const len of [1, 2, 63, 64, 65, 1000, 4096, 65536]) {
        const a = Int32Array.from({ length: len }, () =>
            Math.floor((rnd() + 0.5) * 4294967296) - 2147483648);
        let s = 0;
        for (let i = 0; i < len; i++) s += a[i];
        assert(i32Mean(a) === s / len, "i32Mean exact len=" + len);
    }
    {
        const a = Int32Array.from({ length: 1000 }, (_, i) => (i % 2 ? 2147483647 : -2147483648));
        assert(i32Mean(a) === (500 * 2147483647 + 500 * -2147483648) / 1000, "i32Mean extremes exact");
    }
});
__kitS.end();

if (fails) {
    print("test_simd_stats: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_simd_stats failed");
}
print("test_simd_stats: " + n + " assertions, 0 failures");
