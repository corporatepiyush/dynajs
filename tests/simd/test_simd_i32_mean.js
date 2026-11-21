// covers: dyna:simd.i32Mean
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_mean");
import {
    dot, sum, scale, axpy, add,
    normL1, normL2, max, min, argmax, argmin,
    sub, mul, div, abs, fma,
    addScalar, affine,
    sigmoid, relu, relu6, leakyRelu, elu, tanhFast, gelu, silu,
    softmax, logSoftmax,
    vexp, vlog, vsqrt, vrsqrt, vinv,
    distL2, distL1, distCos, distCheb,
    gemv, gemvT, gemm,
    mean, variance, normalize,
    f64Mean, f64Variance, i32Mean,
    clamp, threshold, topkIndices, cummax,
} from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
const near = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-4);
const approx = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-2);
const f32 = (arr) => Float32Array.from(arr, Math.fround);
const rndVec = (len, seed) => Float32Array.from({ length: len }, (_, i) => Math.fround(((i * 7 + seed) % 17) - 8.5));
const posVec = (len, seed) => Float32Array.from({ length: len }, (_, i) => Math.fround(((i * 5 + seed) % 13) + 0.25));

const SIZES = [0, 1, 3, 4, 7, 8, 15, 16, 100, 1000];

__kitS.section("mean len", () => {
    const f = (a) => Float32Array.from(a, Math.fround);
    const SIZES = [1, 2, 3, 7, 8, 15, 16, 63, 64, 65, 100, 1000];
    for (const len of SIZES) {
        const a = rndVec(len, 3);
        let s = 0;
        for (let i = 0; i < len; i++) s += a[i];
        const m = s / len;
        let v = 0;
        for (let i = 0; i < len; i++) v += (a[i] - m) * (a[i] - m);
        v /= len;
        assert(near(mean(a), m), "mean len=" + len);
        assert(near(variance(a), v), "variance len=" + len);

        const c = Float32Array.from(a);
        assert(normalize(c) === c, "normalize returns a");
        if (len === 1) {
            assert(isNaN(c[0]), "normalize len=1 is NaN (0/0)");
        } else {
            let ns = 0, nv = 0;
            for (let i = 0; i < len; i++) ns += c[i];
            for (let i = 0; i < len; i++) nv += c[i] * c[i];
            assert(near(ns / len, 0), "normalize mean 0 len=" + len);
            assert(near(nv / len, 1), "normalize variance 1 len=" + len);
        }

        const d = Float64Array.from(a);
        assert(near(f64Mean(d), m), "f64Mean len=" + len);
        assert(near(f64Variance(d), v, Math.max(1, v) * 1e-6), "f64Variance len=" + len);
        const ints = Int32Array.from({ length: len }, (_, i2) => (i2 % 7) - 3);
        let is = 0;
        for (let i = 0; i < len; i++) is += ints[i];
        assert(i32Mean(ints) === is / len, "i32Mean exact len=" + len);
    }

    for (const [nm, fn] of [
        ["mean", () => mean(new Float32Array(0))],
        ["variance", () => variance(new Float32Array(0))],
        ["normalize", () => normalize(new Float32Array(0))],
        ["f64Mean", () => f64Mean(new Float64Array(0))],
        ["f64Variance", () => f64Variance(new Float64Array(0))],
        ["i32Mean", () => i32Mean(new Int32Array(0))],
    ]) {
        let t = false; try { fn(); } catch { t = true; }
        assert(t, nm + ": empty array throws");
    }

    for (const at of [0, 1, 63]) {
        const a = rndVec(100, 5);
        a[at] = NaN;
        assert(isNaN(mean(a)), "mean NaN at " + at);
        assert(isNaN(variance(a)), "variance NaN at " + at);
        const c = Float32Array.from(a);
        normalize(c);
        let allNaN = true;
        for (let i = 0; i < 100; i++) if (!isNaN(c[i])) { allNaN = false; break; }
        assert(allNaN, "normalize NaN at " + at + " poisons everything");
    }

    {
        const big = Int32Array.from({ length: 4 * 1024 * 1024 }, () => 2147483647);
        assert(i32Mean(big) === 2147483647, "i32Mean of 4M x INT32_MAX is exact");
        const mixed = new Int32Array([2147483647, -2147483648, 1]);
        assert(i32Mean(mixed) === (2147483647 - 2147483648 + 1) / 3, "i32Mean mixed extremes");
    }

    {
        let t = false;
        try { i32Mean(new Float32Array(4)); } catch { t = true; }
        assert(t, "i32Mean refuses Float32Array");
    }
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
