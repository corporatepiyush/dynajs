// covers: dyna:simd.relu6
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_relu6");
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

__kitS.section("relu6 keeps a NaN element NaN and clamps the rest", () => {
    const a = Float32Array.from([NaN, -1, 3, 7]);
    relu6(a);
    assert(isNaN(a[0]) && a[1] === 0 && a[2] === 3 && a[3] === 6,
        "relu6 keeps a NaN element NaN and clamps the rest");
    const b = Float32Array.from([NaN, -5, 5, 99, -1]);
    clamp(b, -1, 10);
    assert(isNaN(b[0]) && b[1] === -1 && b[2] === 5 && b[3] === 10 && b[4] === -1,
        "clamp keeps a NaN element NaN");
    const c = Float32Array.from([NaN, 1, 2, 3, 4]);
    const s = softmax(c);
    let allNaN = true;
    for (let i = 0; i < s.length; i++) if (!isNaN(s[i])) allNaN = false;
    assert(allNaN, "softmax with a NaN input answers all-NaN");
    const d = Float32Array.from([NaN, 0, 1, Math.E]);
    vlog(d);
    assert(isNaN(d[0]) && d[1] < -3e38 && Math.abs(d[2]) < 1e-6 && Math.abs(d[3] - 1) < 1e-3,
        "vlog keeps a NaN element NaN and answers -FLT_MAX for 0");
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
