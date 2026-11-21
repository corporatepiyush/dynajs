// covers: dyna:simd.sub
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_sub");
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

__kitS.section("sub returns out", () => {
for (const len of SIZES) {
    const a = rndVec(len, 2), b = posVec(len, 3);

    const o1 = new Float32Array(len); const r1 = sub(o1, a, b);
    assert(r1 === o1, "sub returns out");
    for (let i = 0; i < len; i++) assert(o1[i] === Math.fround(a[i] - b[i]), "sub[" + i + "]");

    const o2 = new Float32Array(len); mul(o2, a, b);
    for (let i = 0; i < len; i++) assert(o2[i] === Math.fround(a[i] * b[i]), "mul[" + i + "]");

    const o3 = new Float32Array(len); div(o3, a, b);
    for (let i = 0; i < len; i++) assert(near(o3[i], Math.fround(a[i] / b[i]), Math.max(1, Math.abs(a[i] / b[i])) * 5e-3), "div[" + i + "]");

    const o4 = new Float32Array(len); abs(o4, a);
    for (let i = 0; i < len; i++) assert(o4[i] === Math.fround(Math.abs(a[i])), "abs[" + i + "]");

    const z = Float32Array.from(a); const r5 = fma(z, a, b);
    assert(r5 === z, "fma returns z");
    for (let i = 0; i < len; i++) assert(near(z[i], Math.fround(a[i] + a[i] * b[i]), Math.max(1, Math.abs(a[i])) * 1e-4), "fma[" + i + "]");

    if (len > 0) {
        let threw = false; try { sub(new Float32Array(len), a, new Float32Array(len + 1)); } catch { threw = true; }
        assert(threw, "sub length mismatch throws len=" + len);
    }
}
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
