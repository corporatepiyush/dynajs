// covers: dyna:simd.gemv
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gemv_stats");
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

__kitS.section("gemv 7-arg returns y", () => {
    for (const [m, nn] of [[1, 1], [2, 3], [7, 8], [8, 7], [16, 16], [33, 65], [64, 63]]) {
        const A = randF32(m * nn);
        const x = randF32(nn);
        const y = randF32(m);
        const alpha = Math.fround(rnd() * 4 - 2), beta = Math.fround(rnd() * 4 - 2);
        const want = new Array(m);
        for (let i = 0; i < m; i++) {
            let s = 0;
            for (let k = 0; k < nn; k++) s += A[i * nn + k] * x[k];
            want[i] = alpha * s + beta * y[i];
        }
        const out = Float32Array.from(y);
        assert(gemv(out, A, x, m, nn, alpha, beta) === out, "gemv 7-arg returns y");
        for (let i = 0; i < m; i++)
            assert(near(out[i], want[i], Math.max(1, Math.abs(want[i])) * 1e-3),
                   "gemv alpha/beta m=" + m + " n=" + nn + " i=" + i);
        const poison = new Float32Array(m).fill(NaN);
        gemv(poison, A, x, m, nn, alpha, 0);
        for (let i = 0; i < m; i++)
            assert(!isNaN(poison[i]), "gemv beta=0 ignores y m=" + m + " n=" + nn + " i=" + i);
    }
});
__kitS.end();

if (fails) {
    print("test_simd_stats: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_simd_stats failed");
}
print("test_simd_stats: " + n + " assertions, 0 failures");
