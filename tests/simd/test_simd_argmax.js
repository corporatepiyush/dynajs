// covers: dyna:simd.argmax
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_argmax");
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

for (const len of SIZES) {
    if (len === 0) continue;
    const a = Float32Array.from({ length: len }, (_, i) => Math.fround(i - len / 2 + ((i * 2654435761) % 1000) / 100000));
    let mx = a[0], mn = a[0], amx = 0, amn = 0;
    for (let i = 1; i < len; i++) {
        if (a[i] > mx) { mx = a[i]; amx = i; }
        if (a[i] < mn) { mn = a[i]; amn = i; }
    }
    assert(argmax(a) === amx, "argmax exact index len=" + len);
    assert(argmin(a) === amn, "argmin exact index len=" + len);
}

__kitS.section("argmax answers the first occurrence of a cross-lane tie", () => {
    const tie = Float32Array.from([1, 1, 9, 1, 9, 1, 1, 1]);
    assert(argmax(tie) === 2, "argmax answers the first occurrence of a cross-lane tie");
    const allEq = Float32Array.from([5, 5, 5, 5]);
    assert(argmax(allEq) === 0 && argmin(allEq) === 0,
        "argmax/argmin answer index 0 when every element ties");
    const allEqLong = new Float32Array(11);
    assert(argmax(allEqLong) === 0 && argmin(allEqLong) === 0,
        "argmax/argmin answer index 0 on a long all-equal input");
    const neg = Float32Array.from([-1, -9, -1, -9, -9]);
    assert(argmin(neg) === 1, "argmin answers the first occurrence of a tie");
    const mx = Float32Array.from([2, 8, 8, 8, 3, 8, 8, 8, 8]);
    assert(argmax(mx) === 1, "argmax answers the first occurrence beyond one vector");
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
