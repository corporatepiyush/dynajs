// covers: dyna:simd.gemv
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_gemv");
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

for (const [m, k, nn] of [[1, 1, 1], [2, 3, 4], [5, 1, 7], [8, 8, 8], [16, 5, 3]]) {
    const A = rndVec(m * nn, 11);
    const x = rndVec(nn, 12);
    const y0 = rndVec(m, 13);

    const y = Float32Array.from(y0);
    const ry = gemv(y, A, x, m, nn, 0.5);
    assert(ry === y, "gemv returns y");
    for (let i = 0; i < m; i++) {
        let acc = 0;
        for (let j = 0; j < nn; j++) acc += A[i * nn + j] * x[j];
        assert(near(y[i], 0.5 * y0[i] + acc, Math.max(1, Math.abs(acc)) * 1e-3), "gemv[" + i + "] m=" + m + " n=" + nn);
    }

    const xT = rndVec(m, 14);
    const yT0 = rndVec(nn, 15);
    const yT = Float32Array.from(yT0);
    gemvT(yT, A, xT, m, nn, 0.25);
    for (let j = 0; j < nn; j++) {
        let acc = 0;
        for (let i = 0; i < m; i++) acc += A[i * nn + j] * xT[i];
        assert(near(yT[j], 0.25 * yT0[j] + acc, Math.max(1, Math.abs(acc)) * 1e-3), "gemvT[" + j + "] m=" + m + " n=" + nn);
    }

    const Amk = rndVec(m * k, 16), Bkn = rndVec(k * nn, 17), C0 = rndVec(m * nn, 18);
    const C = Float32Array.from(C0);
    const rc = gemm(C, Amk, Bkn, m, nn, k, 2, 0.5);
    assert(rc === C, "gemm returns c");
    for (let i = 0; i < m; i++) {
        for (let j = 0; j < nn; j++) {
            let acc = 0;
            for (let kk = 0; kk < k; kk++) acc += Amk[i * k + kk] * Bkn[kk * nn + j];
            const want = 2 * acc + 0.5 * C0[i * nn + j];
            assert(near(C[i * nn + j], want, Math.max(1, Math.abs(want)) * 1e-3), "gemm[" + i + "," + j + "] m=" + m + " n=" + nn + " k=" + k);
        }
    }
}

__kitS.section("gemv valid dims does not throw", () => {
    let threw = false;
    try { gemv(new Float32Array(3), new Float32Array(6), new Float32Array(2), 3, 2, 0); } catch { threw = true; }
    assert(!threw, "gemv valid dims does not throw");
    threw = false;
    try { gemv(new Float32Array(3), new Float32Array(5), new Float32Array(2), 3, 2, 0); } catch { threw = true; }
    assert(threw, "gemv: a.length mismatch throws");

    let calls = 0;
    const y = new Float32Array(2);
    gemv(y, new Float32Array(6), new Float32Array(3), { valueOf() { calls++; return 2; } }, { valueOf() { calls++; return 3; } }, { valueOf() { calls++; return 0; } });
    assert(calls === 3, "gemv coerces m,n,beta before resolving buffers");
});

__kitS.section("gemv 6-arg returns y", () => {
    const f = (a) => Float32Array.from(a, Math.fround);
    const A = f([1, 2, 3, 4, 5, 6]);
    const x = f([1, 0, 2]);
    const y6 = f([100, 200]);
    assert(gemv(y6, A, x, 2, 3, 2) === y6, "gemv 6-arg returns y");
    for (let i = 0; i < 2; i++)
        assert(near(y6[i], [207, 416][i]), "gemv 6-arg beta*y + A*x [" + i + "]");

    const y7 = f([100, 200]);
    gemv(y7, A, x, 2, 3, 1, 2);
    for (let i = 0; i < 2; i++)
        assert(near(y7[i], y6[i]), "gemv 7-arg alpha=1 == 6-arg form [" + i + "]");

    const y8 = f([5, 7]);
    gemv(y8, A, x, 2, 3, 3, 0);
    for (let i = 0; i < 2; i++)
        assert(near(y8[i], [21, 48][i]), "gemv 7-arg beta=0 ignores y [" + i + "]");

    const y9 = f([NaN, NaN]);
    gemv(y9, A, x, 2, 3, 2, 0);
    for (let i = 0; i < 2; i++)
        assert(y9[i] === [14, 32][i], "gemv 7-arg beta=0 vs poisoned y [" + i + "]");

    const y10 = f([2, 3]);
    gemv(y10, A, x, 2, 3, -1, 0.5);
    for (let i = 0; i < 2; i++)
        assert(near(y10[i], [-6, -14.5][i]), "gemv 7-arg mixed alpha/beta [" + i + "]");

    {
        const An = f([NaN, 2, 3, 4, 5, 6]);
        const yA = f([0, 0]);
        gemv(yA, An, x, 2, 3, 1, 0);
        assert(isNaN(yA[0]) && yA[1] === 16, "gemv 7-arg NaN in A propagates");
        const yB = f([0, 0]);
        gemv(yB, An, x, 2, 3, 0);
        assert(isNaN(yB[0]) && yB[1] === 16, "gemv 6-arg NaN in A propagates");
    }

    {
        const y = f([0, 0]);
        let t = 0;
        try { gemv(y, A, x, 2, 4, 1, 0); } catch { t = 1; }
        assert(t, "gemv 7-arg a.length != m*n throws");
        t = 0;
        try { gemv(f([0, 0, 0]), A, x, 2, 3, 1, 0); } catch { t = 1; }
        assert(t, "gemv 7-arg y.length != m throws");
    }
});
__kitS.end();

print("test_simd: all tests passed (" + n + " assertions)");
