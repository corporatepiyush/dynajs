// covers: dyna:simd.max
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_max");
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

__kitS.section("normL1 len", () => {
for (const len of SIZES) {
    const a = rndVec(len, 1);

    let l1 = 0, l2sq = 0;
    for (let i = 0; i < len; i++) { l1 += Math.abs(a[i]); l2sq += a[i] * a[i]; }
    assert(near(normL1(a), l1), "normL1 len=" + len);
    assert(near(normL2(a), Math.sqrt(l2sq)), "normL2 len=" + len);

    if (len === 0) {
        for (const [name, fn] of [["max", max], ["min", min], ["argmax", argmax], ["argmin", argmin]]) {
            let threw = false; try { fn(a); } catch { threw = true; }
            assert(threw, name + ": empty array throws");
        }
        continue;
    }

    let mx = a[0], mn = a[0];
    for (let i = 1; i < len; i++) {
        if (a[i] > mx) mx = a[i];
        if (a[i] < mn) mn = a[i];
    }
    assert(max(a) === mx, "max len=" + len);
    assert(min(a) === mn, "min len=" + len);
    const ai = argmax(a), ii = argmin(a);
    assert(ai < len && a[ai] === mx, "argmax value len=" + len);
    assert(ii < len && a[ii] === mn, "argmin value len=" + len);
}
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
