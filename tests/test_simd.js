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

for (const len of SIZES) {
    const a = rndVec(len, 4);

    const c1 = Float32Array.from(a); const r1 = addScalar(c1, 1.75);
    assert(r1 === c1, "addScalar returns a");
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(a[i] + 1.75), "addScalar[" + i + "]");

    const c2 = Float32Array.from(a); affine(c2, 2.5, -0.5);
    for (let i = 0; i < len; i++) assert(c2[i] === Math.fround(2.5 * a[i] - 0.5), "affine[" + i + "]");
}

{
    const a = new Float32Array([1, 2, 3, 4]);
    let calls = 0;
    addScalar(a, { valueOf() { calls++; return 10; } });
    assert(calls === 1 && a[0] === 11 && a[3] === 14, "addScalar coerces scalar first");

    const b = new Float32Array([1, 2, 3, 4]);
    calls = 0;
    affine(b, { valueOf() { calls++; return 2; } }, { valueOf() { calls++; return 1; } });
    assert(calls === 2 && b[0] === 3 && b[3] === 9, "affine coerces both scalars first");
}

for (const len of SIZES) {
    const a = rndVec(len, 5);

    const c1 = Float32Array.from(a); relu(c1);
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(Math.max(0, a[i])), "relu[" + i + "]");

    const c2 = Float32Array.from(a); relu6(c2);
    for (let i = 0; i < len; i++) assert(c2[i] === Math.fround(Math.min(6, Math.max(0, a[i]))), "relu6[" + i + "]");

    const c3 = Float32Array.from(a); leakyRelu(c3, 0.25);
    for (let i = 0; i < len; i++) assert(c3[i] === Math.fround(a[i] > 0 ? a[i] : a[i] * 0.25), "leakyRelu[" + i + "]");

    const c4 = Float32Array.from(a); elu(c4, 1.0);
    for (let i = 0; i < len; i++) {
        const want = a[i] > 0 ? a[i] : Math.fround(Math.exp(a[i]) - 1);
        assert(near(c4[i], want, Math.max(1, Math.abs(want)) * 1e-3), "elu[" + i + "]");
    }

    const c5 = Float32Array.from(a); gelu(c5);
    for (let i = 0; i < len; i++) {
        const x = a[i];
        const want = 0.5 * x * (1 + Math.tanh(0.7978845608028654 * (x + 0.044715 * x * x * x)));
        assert(near(c5[i], want, Math.max(1, Math.abs(want)) * 1e-3), "gelu[" + i + "]");
    }

}

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

for (const len of SIZES) {
    const a = rndVec(len, 7), p = posVec(len, 8);

    const c1 = Float32Array.from(a); vexp(c1);
    for (let i = 0; i < len; i++) assert(approx(c1[i], Math.exp(a[i]), Math.max(1, Math.abs(Math.exp(a[i]))) * 1e-1), "vexp[" + i + "]");

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

for (const len of SIZES) {
    const a = rndVec(len, 9), b = rndVec(len, 10);
    let l1 = 0, l2sq = 0, cheb = 0, dotv = 0, na = 0, nb = 0;
    for (let i = 0; i < len; i++) {
        const df = a[i] - b[i];
        l1 += Math.abs(df); l2sq += df * df; cheb = Math.max(cheb, Math.abs(df));
        dotv += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i];
    }
    assert(near(distL2(a, b), Math.sqrt(l2sq)), "distL2 len=" + len);
    assert(near(distL1(a, b), l1), "distL1 len=" + len);
    assert(near(distCheb(a, b), cheb), "distCheb len=" + len);
    const denom = Math.sqrt(na * nb);
    const wantCos = denom < 1e-30 ? 1 : 1 - dotv / denom;
    assert(near(distCos(a, b), wantCos, 1e-3), "distCos len=" + len);
}

{
    const big = new Float32Array(8).fill(1e10);
    const neg = new Float32Array(8).fill(-1e10);
    assert(Math.abs(distCos(big, big)) < 1e-6,
           "distCos at the float-product overflow edge (identical) -> " +
           distCos(big, big));
    assert(Math.abs(distCos(big, neg) - 2) < 1e-6,
           "distCos at the float-product overflow edge (antiparallel) -> " +
           distCos(big, neg));
}

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

{
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
}

for (const len of SIZES) {
    const a = rndVec(len, 19);

    const c1 = Float32Array.from(a); clamp(c1, -2, 2);
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(Math.min(2, Math.max(-2, a[i]))), "clamp[" + i + "]");

    const c2 = Float32Array.from(a); threshold(c2, 0);
    for (let i = 0; i < len; i++) assert(c2[i] === (a[i] > 0 ? 1 : 0), "threshold[" + i + "]");
}

for (const [len, k] of [[0, 3], [1, 0], [1, 5], [5, 0], [5, 2], [5, 5], [5, 100], [100, 10]]) {
    const vals = rndVec(len, 20);
    const idx = topkIndices(vals, k);
    const wantK = Math.min(k, len);
    assert(idx.length === wantK, "topkIndices length len=" + len + " k=" + k);
    assert(idx instanceof Uint32Array, "topkIndices returns Uint32Array");

    const got = Array.from(idx, (i) => vals[i]).sort((x, y) => y - x);
    const want = Array.from(vals).sort((x, y) => y - x).slice(0, wantK);
    for (let i = 0; i < wantK; i++) assert(got[i] === want[i], "topkIndices value[" + i + "] len=" + len + " k=" + k);

    const seen = new Set();
    for (const i of idx) { assert(i < len, "topkIndices index in bounds"); assert(!seen.has(i), "topkIndices index unique"); seen.add(i); }
}

{
    let t = false; try { dot(new Float32Array(3), new Float32Array(4)); } catch { t = true; }
    assert(t, "length mismatch throws");
    t = false; try { dot(new Uint8Array(4), new Uint8Array(4)); } catch { t = true; }
    assert(t, "non-Float32Array throws");
    t = false; try { sum([1, 2, 3]); } catch { t = true; }
    assert(t, "plain array throws");
}

{
    const a = new Float32Array([1, 2, 3, 4]);
    let calls = 0;
    scale(a, { valueOf() { calls++; return 2; } });
    assert(calls === 1 && a[0] === 2 && a[3] === 8, "scale coerces scalar first");
}

{
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
    for (let i = 0; i < 4; i++) assert(a[i] === -3.4028234663852886e+38, "vlog NaN -> -FLT_MAX [" + i + "]");

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
}

{
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
}

{
    const f = (a) => Float32Array.from(a, Math.fround);
    const SIZES = [1, 2, 3, 7, 8, 15, 16, 63, 64, 65, 100, 1000];
    for (const len of SIZES) {
        const a = rndVec(len, 3);
        let s = 0;
        for (let i = 0; i < len; i++) s += a[i];
        const m = s / len;
        let v = 0;
        for (let i = 0; i < len; i++) v += (a[i] - m) * (a[i] - m);
        v /= len;
        assert(near(mean(a), m), "mean len=" + len);
        assert(near(variance(a), v), "variance len=" + len);

        const c = Float32Array.from(a);
        assert(normalize(c) === c, "normalize returns a");
        if (len === 1) {
            assert(isNaN(c[0]), "normalize len=1 is NaN (0/0)");
        } else {
            let ns = 0, nv = 0;
            for (let i = 0; i < len; i++) ns += c[i];
            for (let i = 0; i < len; i++) nv += c[i] * c[i];
            assert(near(ns / len, 0), "normalize mean 0 len=" + len);
            assert(near(nv / len, 1), "normalize variance 1 len=" + len);
        }

        const d = Float64Array.from(a);
        assert(near(f64Mean(d), m), "f64Mean len=" + len);
        assert(near(f64Variance(d), v, Math.max(1, v) * 1e-6), "f64Variance len=" + len);
        const ints = Int32Array.from({ length: len }, (_, i2) => (i2 % 7) - 3);
        let is = 0;
        for (let i = 0; i < len; i++) is += ints[i];
        assert(i32Mean(ints) === is / len, "i32Mean exact len=" + len);
    }

    for (const [nm, fn] of [
        ["mean", () => mean(new Float32Array(0))],
        ["variance", () => variance(new Float32Array(0))],
        ["normalize", () => normalize(new Float32Array(0))],
        ["f64Mean", () => f64Mean(new Float64Array(0))],
        ["f64Variance", () => f64Variance(new Float64Array(0))],
        ["i32Mean", () => i32Mean(new Int32Array(0))],
    ]) {
        let t = false; try { fn(); } catch { t = true; }
        assert(t, nm + ": empty array throws");
    }

    for (const at of [0, 1, 63]) {
        const a = rndVec(100, 5);
        a[at] = NaN;
        assert(isNaN(mean(a)), "mean NaN at " + at);
        assert(isNaN(variance(a)), "variance NaN at " + at);
        const c = Float32Array.from(a);
        normalize(c);
        let allNaN = true;
        for (let i = 0; i < 100; i++) if (!isNaN(c[i])) { allNaN = false; break; }
        assert(allNaN, "normalize NaN at " + at + " poisons everything");
    }

    {
        const big = Int32Array.from({ length: 4 * 1024 * 1024 }, () => 2147483647);
        assert(i32Mean(big) === 2147483647, "i32Mean of 4M x INT32_MAX is exact");
        const mixed = new Int32Array([2147483647, -2147483648, 1]);
        assert(i32Mean(mixed) === (2147483647 - 2147483648 + 1) / 3, "i32Mean mixed extremes");
    }

    {
        let t = false;
        try { i32Mean(new Float32Array(4)); } catch { t = true; }
        assert(t, "i32Mean refuses Float32Array");
    }
}

print("test_simd: all tests passed (" + n + " assertions)");
