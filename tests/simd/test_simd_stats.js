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

{
    for (const off of [1e4, 1e6]) {
        for (const len of [63, 64, 65, 1000]) {
            const a = Float32Array.from({ length: len }, (_, i) => Math.fround(off + rnd() * 1e-3));
            const m = oracleMean(a), v = oracleVar(a);
            assert(near(mean(a), m), "offset " + off + " mean len=" + len);
            assert(Math.abs(variance(a) - v) <= Math.abs(v) * 0.5 + 1e-18,
                   "offset " + off + " variance len=" + len + " (var=" + v + ")");
        }
    }
}

{
    for (const v of [0, 1, 8.5, -3.25]) {
        for (const len of [1, 2, 63, 64, 65, 1000]) {
            const c = new Float32Array(len).fill(v);
            normalize(c);
            let allNaN = true, bounded = true;
            for (let i = 0; i < len; i++) {
                if (!isNaN(c[i])) allNaN = false;
                if (!(Math.abs(c[i]) <= 1 + 1e-6 || isNaN(c[i]))) bounded = false;
            }
            assert(allNaN || bounded, "constant " + v + " len=" + len + " has no meaningful z-score");
        }
    }
    {
        assert(f64Variance(new Float64Array(1000).fill(8.5)) === 0, "f64Variance constant = 0");
        assert(f64Variance(new Float64Array([7])) === 0, "f64Variance len=1 = 0");
        assert(f64Mean(new Float64Array([7])) === 7, "f64Mean len=1 exact");
        assert(f64Variance(new Float64Array([7])) === 0, "f64Variance len=1 pinned");
    }
}

{
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
}

{
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
}

{
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
}

if (fails) {
    print("test_simd_stats: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_simd_stats failed");
}
print("test_simd_stats: " + n + " assertions, 0 failures");
