// covers: dyna:simd.axpy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_axpy");
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

__kitS.section("dot len=", () => {
for (const len of SIZES) {
    const a = Float32Array.from({ length: len }, (_, i) => Math.fround((i % 17) - 8.5));
    const b = Float32Array.from({ length: len }, (_, i) => Math.fround((i % 13) - 6.25));
    let d = 0, s = 0;
    for (let i = 0; i < len; i++) { d += a[i] * b[i]; s += a[i]; }
    assert(near(dot(a, b), d), "dot len=" + len);
    assert(near(sum(a), s), "sum len=" + len);

    const y = Float32Array.from(a), x = Float32Array.from(b);
    const r = axpy(y, 2.5, x);
    assert(r === y, "axpy returns y");
    for (let i = 0; i < len; i++) assert(near(y[i], Math.fround(a[i] + 2.5 * b[i])), "axpy[" + i + "]");

    const c = Float32Array.from(a); scale(c, -3);
    for (let i = 0; i < len; i++) assert(near(c[i], Math.fround(a[i] * -3)), "scale[" + i + "]");

    const o = new Float32Array(len); add(o, a, b);
    for (let i = 0; i < len; i++) assert(near(o[i], Math.fround(a[i] + b[i])), "add[" + i + "]");
}
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
