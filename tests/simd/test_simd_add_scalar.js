// covers: dyna:simd.addScalar
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_add_scalar");
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
    const a = rndVec(len, 4);

    const c1 = Float32Array.from(a); const r1 = addScalar(c1, 1.75);
    assert(r1 === c1, "addScalar returns a");
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(a[i] + 1.75), "addScalar[" + i + "]");

    const c2 = Float32Array.from(a); affine(c2, 2.5, -0.5);
    for (let i = 0; i < len; i++) assert(c2[i] === Math.fround(2.5 * a[i] - 0.5), "affine[" + i + "]");
}

__kitS.section("addScalar coerces scalar first", () => {
    const a = new Float32Array([1, 2, 3, 4]);
    let calls = 0;
    addScalar(a, { valueOf() { calls++; return 10; } });
    assert(calls === 1 && a[0] === 11 && a[3] === 14, "addScalar coerces scalar first");

    const b = new Float32Array([1, 2, 3, 4]);
    calls = 0;
    affine(b, { valueOf() { calls++; return 2; } }, { valueOf() { calls++; return 1; } });
    assert(calls === 2 && b[0] === 3 && b[3] === 9, "affine coerces both scalars first");
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
