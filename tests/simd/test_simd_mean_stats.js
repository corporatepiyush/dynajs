// covers: dyna:simd.mean
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_mean_stats");
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

for (const len of SIZES) {
    const a = randF32(len);
    const m = oracleMean(a), v = oracleVar(a);
    assert(near(mean(a), m), "f32 mean len=" + len);
    assert(near(variance(a), v), "f32 variance len=" + len);

    const d = randF64(len);
    const dm = oracleMean(d), dv = oracleVar(d);
    assert(near(f64Mean(d), dm, Math.max(1, Math.abs(dm)) * 1e-12), "f64 mean len=" + len);
    assert(near(f64Variance(d), dv, Math.max(1, dv) * 1e-12), "f64 variance len=" + len);

    if (len === 1) continue;
    const c = Float32Array.from(a);
    normalize(c);
    const nz = oracleMean(c), nvz = oracleVar(c);
    assert(near(nz, 0, 1e-3), "normalize centered mean len=" + len);
    assert(near(nvz, 1, 1e-3), "normalize unit variance len=" + len);
    const std = Math.sqrt(v);
    for (let i = 0; i < len; i++)
        assert(near(c[i], (a[i] - m) / std, Math.max(1, 1) * 2e-3), "normalize z len=" + len + " i=" + i);
}

__kitS.section("offset", () => {
    for (const off of [1e4, 1e6]) {
        for (const len of [63, 64, 65, 1000]) {
            const a = Float32Array.from({ length: len }, (_, i) => Math.fround(off + rnd() * 1e-3));
            const m = oracleMean(a), v = oracleVar(a);
            assert(near(mean(a), m), "offset " + off + " mean len=" + len);
            assert(Math.abs(variance(a) - v) <= Math.abs(v) * 0.5 + 1e-18,
                   "offset " + off + " variance len=" + len + " (var=" + v + ")");
        }
    }
});

__kitS.section("mean NaN at", () => {
    for (const at of [0, 50, 999]) {
        const a = randF32(1000);
        a[at] = NaN;
        assert(isNaN(mean(a)), "mean NaN at " + at);
        assert(isNaN(variance(a)), "variance NaN at " + at);
    }
    {
        const allNaN = new Float32Array(64).fill(NaN);
        assert(isNaN(mean(allNaN)) && isNaN(variance(allNaN)), "all-NaN f32");
        assert(isNaN(f64Mean(new Float64Array(8).fill(NaN))), "all-NaN f64 mean");
        assert(isNaN(f64Variance(new Float64Array(8).fill(NaN))), "all-NaN f64 variance");
    }
    {
        const a = randF32(32);
        a[7] = Infinity;
        assert(mean(a) === Infinity, "single +Inf -> mean = +Inf");
        assert(isNaN(variance(a)), "variance with +Inf -> NaN (Inf-mean is Inf, squared sum Inf, centering Inf-Inf at i=7)");
    }
});
__kitS.end();

if (fails) {
    print("test_simd_stats: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_simd_stats failed");
}
print("test_simd_stats: " + n + " assertions, 0 failures");
