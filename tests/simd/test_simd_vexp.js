// covers: dyna:simd.vexp
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_vexp");
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

__kitS.section("vexp over the whole normal range within 2 ulp", () => {
    const xs = [], step = 0.0137;
    for (let x = -87.9; x <= 88.7; x += step) xs.push(x);
    xs.push(-88, -87.336, -1e-30, 0, 1e-30, 0.5, 1, 2, Math.LN2, 88.72, 88.7228);
    const got = vexp(Float32Array.from(xs));
    let worst = 0, at = 0;
    for (let i = 0; i < xs.length; i++) {
        const x = Math.fround(xs[i]), want = Math.exp(x);
        if (want < 1.1754944e-38) continue;
        const err = Math.abs(got[i] - want) / (want * 1.1920929e-7);
        if (err > worst) { worst = err; at = x; }
    }
    assert(worst <= 2, "vexp over the whole normal range within 2 ulp (worst " + worst.toFixed(2) + " ulp at " + at + ")");
    assert(vexp(Float32Array.from([1]))[0] === Math.fround(Math.E), "vexp(1) is fround(e)");
    const sub = vexp(Float32Array.from([-87.9]))[0];
    assert(sub > 0 && Math.abs(sub - Math.exp(-87.9)) < 1e-44 * 4, "vexp just above -88 is a subnormal, not 0 (" + sub + ")");
});
__kitS.end();

for (const len of SIZES) {
    const a = rndVec(len, 7), p = posVec(len, 8);

    const c1 = Float32Array.from(a); vexp(c1);
    for (let i = 0; i < len; i++) {
        const want = Math.exp(a[i]);
        assert(Math.abs(c1[i] - want) <= 4 * 1.1920929e-7 * want, "vexp[" + i + "] within 4 ulp of exp (" + c1[i] + " vs " + want + ")");
    }

    const c2 = Float32Array.from(p); vlog(c2);
    for (let i = 0; i < len; i++) {
        const want = Math.fround(Math.log(p[i]));
        const ulp = Math.max(Math.abs(want) * 1.1920929e-7, 1e-45);
        assert(Math.abs(c2[i] - want) <= ulp, "vlog[" + i + "]");
    }

    const c3 = Float32Array.from(p); vsqrt(c3);
    for (let i = 0; i < len; i++) assert(near(c3[i], Math.fround(Math.sqrt(p[i])), Math.max(1, Math.abs(Math.sqrt(p[i]))) * 1e-5), "vsqrt[" + i + "]");

    const c4 = Float32Array.from(p); vrsqrt(c4);
    for (let i = 0; i < len; i++) assert(near(c4[i], 1 / Math.sqrt(p[i]), Math.max(1, Math.abs(1 / Math.sqrt(p[i]))) * 1e-6), "vrsqrt[" + i + "]");

    const c5 = Float32Array.from(p); vinv(c5);
    for (let i = 0; i < len; i++) assert(near(c5[i], Math.fround(1 / p[i]), Math.max(1, Math.abs(1 / p[i])) * 1e-5), "vinv[" + i + "]");
}

print("test_simd: all tests passed (" + n + " assertions)");
