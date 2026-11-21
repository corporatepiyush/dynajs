// covers: dyna:simd.sigmoid
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_sigmoid");
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

__kitS.section("vexp NaN propagates", () => {
    const f = (a) => Float32Array.from(a, Math.fround);
    const isn4 = (v) => typeof v === "number" && Number.isNaN(v);

    let a = f([NaN, NaN, NaN, NaN]); vexp(a);
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "vexp NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); sigmoid(a);
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "sigmoid NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); tanhFast(a);
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "tanhFast NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); relu(a);
    /* CONTRACT CHANGE (FIX-4): the module doc pins "NaN in the input propagates to
       NaN through every reduction and in-place transform" -- relu/vsqrt/vrsqrt
       previously clamped NaN to 0. */
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "relu NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); vsqrt(a);
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "vsqrt NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); vrsqrt(a);
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "vrsqrt NaN propagates [" + i + "]");
    a = f([NaN, NaN, NaN, NaN]); vlog(a);
    /* COMPAT-040: the same "NaN propagates" clause covers vlog; the SIMD lane
       kernel used to map NaN to -FLT_MAX while the scalar reference returned NaN. */
    for (let i = 0; i < 4; i++) assert(isn4(a[i]), "vlog NaN propagates [" + i + "]");
    a = f([NaN, 0, 1, 4]); vlog(a);
    assert(isn4(a[0]) && a[1] === -3.4028234663852886e+38 && a[2] === 0,
        "vlog NaN propagates while 0 still answers -FLT_MAX");

    a = f([0.7, -1.3, 2.1, -0.4, 0.7]);
    sigmoid(a);
    assert(a[0] === a[4], "sigmoid head==tail bitwise");
    a = f([0.7, -1.3, 2.1, -0.4, 0.7]);
    tanhFast(a);
    assert(a[0] === a[4], "tanhFast head==tail bitwise");

    a = f([-50, -31, 31, 50]); sigmoid(a);
    assert(a[0] === 0 && a[1] === 0 && a[2] === 1 && a[3] === 1, "sigmoid saturation exact");
    a = f([-50, -11, 11, 50]); tanhFast(a);
    assert(a[0] === -1 && a[1] === -1 && a[2] === 1 && a[3] === 1, "tanhFast saturation exact");
    a = f([-100, 100]); vexp(a);
    assert(a[0] === 0 && a[1] === Infinity, "vexp saturation exact");

    a = f([-100, -89.5]); silu(a);
    assert(Math.abs(a[0]) < 1e-6 && Math.abs(a[1]) < 1e-6, "silu large-negative ~0");
    a = f([-Infinity, 0]); silu(a);
    assert(isn4(a[0]) && a[1] === 0, "silu(-Inf) = NaN (scalar parity)");

    a = f([-2, -0, 0, -Infinity]); vrsqrt(a);
    assert(a[0] === 0 && a[1] === 0 && a[2] === 0 && a[3] === 0, "vrsqrt non-positive -> 0");

    /* CONTRACT CHANGE (FIX-4): "NaN in the input propagates to NaN through every
       reduction" — max/min previously skipped NaN lanes. */
    assert(Number.isNaN(max(f([NaN, 1, 2, 3]))), "max NaN poisons");
    assert(Number.isNaN(min(f([NaN, 1, 2, 3]))), "min NaN poisons");
    assert(Number.isNaN(max(f([NaN, NaN, NaN, NaN]))), "max all-NaN = NaN");
    assert(Number.isNaN(min(f([NaN, NaN, NaN, NaN]))), "min all-NaN = NaN");
    {
        const v = f([1, NaN, 7, 1, 1, 9, 1, 1]);
        assert(argmax(v) === 5, "argmax skips NaN, finds cross-block max");
        const w = f([5, NaN, 7, 1, -9, 1, 1, 1]);
        assert(argmin(w) === 4, "argmin skips NaN, finds cross-block min");
    }

    {
        const A = f([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
        const ones = f([1, 1, 1, 1]);
        const y = f([NaN, NaN, NaN, NaN]);
        gemv(y, A, ones, 4, 4, 0);
        for (let i = 0; i < 4; i++) assert(y[i] === 16 * i + 10, "gemv beta=0 ignores prior y [" + i + "]");
        const yT = f([NaN, NaN, NaN, NaN]);
        gemvT(yT, A, ones, 4, 4, 0);
        for (let j = 0; j < 4; j++) assert(yT[j] === 28 + 4 * j, "gemvT beta=0 ignores prior y [" + j + "]");
        const c = f([NaN, NaN, NaN, NaN]);
        gemm(c, f([1, 0, 0, 1]), f([2, 0, 0, 2]), 2, 2, 2, 1, 0);
        assert(c[0] === 2 && c[1] === 0 && c[2] === 0 && c[3] === 2, "gemm beta=0 ignores prior c");
        const c2 = f([7, 7, 7, 7]);
        gemm(c2, f([1, 2, 3, 4]), f([5, 6, 7, 8]), 2, 2, 2, 1, 0);
        assert(c2[0] === 19 && c2[1] === 22 && c2[2] === 43 && c2[3] === 50, "gemm beta=0 exact A*B");
    }

    {
        const s1 = f([-Infinity, -Infinity, -Infinity, -Infinity]); softmax(s1);
        for (let i = 0; i < 4; i++) assert(isn4(s1[i]), "softmax all--Inf = NaN (sum 0) [" + i + "]");
        const s2 = f([0, -100, -100, -100]); softmax(s2);
        assert(s2[0] === 1 && s2[1] === 0 && s2[2] === 0 && s2[3] === 0, "softmax one-hot stable");
        const l1 = f([0, -100, -100, -100]); logSoftmax(l1);
        for (let i = 0; i < 4; i++)
            assert(l1[i] > -1e30, "logSoftmax deep-negative finite (analytic logsumexp) [" + i + "]");
    }

    {
        const v = f([1, NaN, 2, 3, 4, 5, 6, 7]);
        cummax(v);
        for (let i = 0; i < 8; i++) assert(v[i] === [1, 1, 2, 3, 4, 5, 6, 7][i], "cummax NaN carry [" + i + "]");
    }

    {
        const z = f([-0, -0, -0, -0]); relu(z);
        for (let i = 0; i < 4; i++) assert(z[i] === 0 && 1 / z[i] === Infinity, "relu(-0) -> +0 [" + i + "]");
        const big = f([1e30, 1e30, 1e30, 1e30]);
        assert(distL2(big, f([-1e30, 1e30, -1e30, 1e30])) === Infinity, "distL2 overflow -> +Inf");
        assert(distCos(new Float32Array(4), new Float32Array(4)) === 1, "distCos zero vectors -> 1");
    }

    {
        const e = new Float32Array(0);
        assert(sum(e) === 0 && dot(e, e) === 0 && normL1(e) === 0 && normL2(e) === 0, "empty reductions = 0");
        assert(distL2(e, e) === 0 && distL1(e, e) === 0 && distCheb(e, e) === 0, "empty distances = 0");
        assert(topkIndices(e, 3).length === 0, "topk empty -> empty");
    }

    {
        const bits = new Int32Array([0x3f800000, 0x40000000, 0x3f800000, 0x40000000]);
        assert(sum(bits) === 6, "sum reinterprets Int32Array bits as f32");
        const o = new Float32Array(4);
        mul(o, bits, new Uint32Array([0x3f800000, 0x40000000, 0x3f800000, 0x40000000]));
        assert(o[0] === 1 && o[1] === 4 && o[3] === 4, "mul over F32Like bit views");
    }

    {
        const t = Array.from(topkIndices(f([5, 5, 5, 5, 5, 5, 5, 5]), 3)).sort();
        assert(t.join(",") === "0,1,2", "topk all-tie keeps lowest indices");
        const t2 = Array.from(topkIndices(f([1, 9, 1, 9, 1, 9, 1, 9]), 4)).sort((x, y) => x - y);
        assert(t2.join(",") === "1,3,5,7", "topk tie set correct");
    }

    {
        const s1 = f([1, 2, 3, 4]); add(s1, s1, s1);
        assert(s1[3] === 8, "add out===a===b");
        const s2 = f([1, 2, 3, 4]); fma(s2, s2, s2);
        assert(s2[3] === 4 + 16, "fma z===a===b");
    }
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
