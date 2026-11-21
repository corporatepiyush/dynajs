// covers: dyna:simd.distCos
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_dist_cos");
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

__kitS.section("distCos at the float-product overflow edge (identical) ->", () => {
    const big = new Float32Array(8).fill(1e10);
    const neg = new Float32Array(8).fill(-1e10);
    assert(Math.abs(distCos(big, big)) < 1e-6,
           "distCos at the float-product overflow edge (identical) -> " +
           distCos(big, big));
    assert(Math.abs(distCos(big, neg) - 2) < 1e-6,
           "distCos at the float-product overflow edge (antiparallel) -> " +
           distCos(big, neg));
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
