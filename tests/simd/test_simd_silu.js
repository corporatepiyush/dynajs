// covers: dyna:simd.silu
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_silu");
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

__kitS.section("sigmoid returns a", () => {
for (const len of SIZES) {
    const a = rndVec(len, 6);

    const c1 = Float32Array.from(a); const r1 = sigmoid(c1);
    assert(r1 === c1, "sigmoid returns a");
    for (let i = 0; i < len; i++) assert(approx(c1[i], 1 / (1 + Math.exp(-a[i])), 2e-2), "sigmoid[" + i + "]");

    const c2 = Float32Array.from(a); tanhFast(c2);
    for (let i = 0; i < len; i++) assert(approx(c2[i], Math.tanh(a[i]), 4e-2), "tanhFast[" + i + "]");

    const c1b = Float32Array.from(a); silu(c1b);
    for (let i = 0; i < len; i++) assert(approx(c1b[i], a[i] / (1 + Math.exp(-a[i])), 2e-2), "silu[" + i + "]");

    if (len > 0) {
        const ck = Float32Array.from(a); silu(ck);
        const cc = Float32Array.from(a); sigmoid(cc);
        for (let i = 0; i < len; i++) cc[i] = a[i] * cc[i];
        for (let i = 0; i < len; i++)
            assert(Math.abs(ck[i] - cc[i]) <= 2e-2,
                   "silu kernel ~ sigmoid+mul [" + i + "]");
        const ci = Float32Array.from(a); silu(ci);
        const kb = new Uint32Array(ck.buffer), ib = new Uint32Array(ci.buffer);
        for (let i = 0; i < len; i++)
            assert(ib[i] === kb[i], "silu in-place == fresh [" + i + "]");
    }

    if (len > 0) {
        const c3 = Float32Array.from(a); softmax(c3);
        let s = 0; const want = a.map((v) => Math.exp(v - Math.max(...a)));
        const wsum = want.reduce((p, v) => p + v, 0);
        for (let i = 0; i < len; i++) s += c3[i];
        assert(near(s, 1, 1e-2), "softmax sums to 1, len=" + len);
        for (let i = 0; i < len; i++) assert(approx(c3[i], want[i] / wsum, 3e-2), "softmax[" + i + "]");

        const c4 = Float32Array.from(a); logSoftmax(c4);
        for (let i = 0; i < len; i++) assert(approx(c4[i], Math.log(want[i] / wsum), Math.max(1, Math.abs(Math.log(want[i] / wsum))) * 1e-1), "logSoftmax[" + i + "]");
    } else {
        let threw = false; try { softmax(Float32Array.from(a)); } catch { threw = true; }
        assert(threw, "softmax: empty array throws");
        threw = false; try { logSoftmax(Float32Array.from(a)); } catch { threw = true; }
        assert(threw, "logSoftmax: empty array throws");
    }
}
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
