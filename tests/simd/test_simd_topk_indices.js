// covers: dyna:simd.topkIndices
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_topk_indices");
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

__kitS.section("topkIndices length len", () => {
for (const [len, k] of [[0, 3], [1, 0], [1, 5], [5, 0], [5, 2], [5, 5], [5, 100], [100, 10]]) {
    const vals = rndVec(len, 20);
    const idx = topkIndices(vals, k);
    const wantK = Math.min(k, len);
    assert(idx.length === wantK, "topkIndices length len=" + len + " k=" + k);
    assert(idx instanceof Uint32Array, "topkIndices returns Uint32Array");

    const got = Array.from(idx, (i) => vals[i]).sort((x, y) => y - x);
    const want = Array.from(vals).sort((x, y) => y - x).slice(0, wantK);
    for (let i = 0; i < wantK; i++) assert(got[i] === want[i], "topkIndices value[" + i + "] len=" + len + " k=" + k);

    const seen = new Set();
    for (const i of idx) { assert(i < len, "topkIndices index in bounds"); assert(!seen.has(i), "topkIndices index unique"); seen.add(i); }
}
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
