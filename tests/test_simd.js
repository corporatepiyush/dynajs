/* test_simd.js — dyna:simd float32 vector kernels.
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_simd.js
 *
 * Tolerance classes (documented per assertion group below):
 *   - exact:  simple elementwise ops (single float op per element) -- fround-exact.
 *   - near:   reduction-style accumulations (dot/sum/norm/dist/gemv/gemm) and
 *             fma (may use a fused multiply-add) -- 1e-4 relative (near()).
 *   - loose:  kernels built on the fast_exp/fast_tanh approximation (sigmoid,
 *             tanhFast, softmax, logSoftmax, vexp) -- 1e-2 relative (approx()).
 */
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
    clamp, threshold, topkIndices,
} from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
const near = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-4);
const approx = (a, b, eps) => Math.abs(a - b) <= (eps || Math.max(1, Math.abs(b)) * 1e-2);
const f32 = (arr) => Float32Array.from(arr, Math.fround);
const rndVec = (len, seed) => Float32Array.from({ length: len }, (_, i) => Math.fround(((i * 7 + seed) % 17) - 8.5));
const posVec = (len, seed) => Float32Array.from({ length: len }, (_, i) => Math.fround(((i * 5 + seed) % 13) + 0.25));

const SIZES = [0, 1, 3, 4, 7, 8, 15, 16, 100, 1000];

/* ---------- original dot/sum/scale/axpy/add coverage (unchanged) ---------- */
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

/* ---------- reductions: normL1, normL2, max, min, argmax, argmin ---------- */
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
    /* argmax/argmin: rndVec repeats with period 17, so long vectors have
     * tied max/min values -- which tied index wins is a SIMD lane-order
     * artifact (block-lane-major, not left-to-right), not a documented
     * contract. Check the weaker, always-true invariant: the value at the
     * returned index equals the true reduction. Index-exactness for a
     * tie-free vector is checked separately below. */
    const ai = argmax(a), ii = argmin(a);
    assert(ai < len && a[ai] === mx, "argmax value len=" + len);
    assert(ii < len && a[ii] === mn, "argmin value len=" + len);
}

/* argmax/argmin exact index, using a strictly-monotonic (tie-free) vector */
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

/* ---------- element-wise vector-vector: sub, mul, div, abs, fma ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 2), b = posVec(len, 3);

    const o1 = new Float32Array(len); const r1 = sub(o1, a, b);
    assert(r1 === o1, "sub returns out");
    for (let i = 0; i < len; i++) assert(o1[i] === Math.fround(a[i] - b[i]), "sub[" + i + "]");

    const o2 = new Float32Array(len); mul(o2, a, b);
    for (let i = 0; i < len; i++) assert(o2[i] === Math.fround(a[i] * b[i]), "mul[" + i + "]");

    const o3 = new Float32Array(len); div(o3, a, b);
    /* div: NEON has no divide instruction and uses a reciprocal-estimate +
     * Newton-Raphson approximation, not exact IEEE division (measured up to
     * ~0.25% relative error) -- near with a wider tolerance, not ===. */
    for (let i = 0; i < len; i++) assert(near(o3[i], Math.fround(a[i] / b[i]), Math.max(1, Math.abs(a[i] / b[i])) * 5e-3), "div[" + i + "]");

    const o4 = new Float32Array(len); abs(o4, a);
    for (let i = 0; i < len; i++) assert(o4[i] === Math.fround(Math.abs(a[i])), "abs[" + i + "]");

    const z = Float32Array.from(a); const r5 = fma(z, a, b);
    assert(r5 === z, "fma returns z");
    for (let i = 0; i < len; i++) assert(near(z[i], Math.fround(a[i] + a[i] * b[i]), Math.max(1, Math.abs(a[i])) * 1e-4), "fma[" + i + "]");

    /* mismatched lengths throw */
    if (len > 0) {
        let threw = false; try { sub(new Float32Array(len), a, new Float32Array(len + 1)); } catch { threw = true; }
        assert(threw, "sub length mismatch throws len=" + len);
    }
}

/* ---------- scalar-vector: addScalar, affine ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 4);

    const c1 = Float32Array.from(a); const r1 = addScalar(c1, 1.75);
    assert(r1 === c1, "addScalar returns a");
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(a[i] + 1.75), "addScalar[" + i + "]");

    const c2 = Float32Array.from(a); affine(c2, 2.5, -0.5);
    for (let i = 0; i < len; i++) assert(c2[i] === Math.fround(2.5 * a[i] - 0.5), "affine[" + i + "]");
}

/* reentrancy: scalar args coerced before the buffer is resolved */
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

/* ---------- activations: exact-libm ones (relu family, elu, gelu, silu) ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 5);

    const c1 = Float32Array.from(a); relu(c1);
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(Math.max(0, a[i])), "relu[" + i + "]");

    const c2 = Float32Array.from(a); relu6(c2);
    for (let i = 0; i < len; i++) assert(c2[i] === Math.fround(Math.min(6, Math.max(0, a[i]))), "relu6[" + i + "]");

    /* slope 0.25 is exactly representable in binary (unlike e.g. 0.1) so the
     * scalar coerces to the identical float32 on both the JS and C side --
     * no double-rounding drift between the reference and the kernel. */
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

    /* silu is tested below with the fast_exp-approximation group: it is
     * composed from sigmoid (see dyna-simd.c for why), not libm-exact. */
}

/* ---------- activations built on fast_exp/fast_tanh: looser tolerance ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 6);

    /* These kernels use the Schraudolph fast_exp/fast_tanh bit-hack, whose
     * error is a few percent worst-case (NOT the header's aspirational ~1e-4).
     * Tolerances are the measured worst-case abs error over these inputs
     * (rndVec, seed 6, len<=1000) plus margin. The measured numbers below are
     * IDENTICAL on SSE4.2 (qemu Nehalem) and AVX2 (qemu Haswell) -- both use
     * the same round-to-nearest cvtps_epi32 formulation:
     *     sigmoid  measured 1.25e-2  -> tol 2e-2 (1.6x)
     *     tanhFast measured 2.31e-2  -> tol 4e-2 (1.7x)
     *     silu     measured 1.06e-2  -> tol 2e-2 (1.9x)
     *     softmax  measured 1.15e-2  -> tol 3e-2 (2.6x)
     *     logSoftmax measured 1.83e-2 rel -> tol 1e-1 (5.5x)
     *     vexp     measured 6.09e-2 rel -> tol 1e-1 (1.6x)
     * (sigmoid/silu/tanhFast bounds are absolute -- outputs are in [-1,1].) */
    const c1 = Float32Array.from(a); const r1 = sigmoid(c1);
    assert(r1 === c1, "sigmoid returns a");
    for (let i = 0; i < len; i++) assert(approx(c1[i], 1 / (1 + Math.exp(-a[i])), 2e-2), "sigmoid[" + i + "]");

    /* tanhFast is an explicit fast approximation (2*fast_sigmoid(2x)-1); its
     * measured worst-case abs error here is 2.31e-2 -- 4e-2 keeps >=1.5x
     * margin. Do NOT tighten toward Math.tanh: the name promises "fast". */
    const c2 = Float32Array.from(a); tanhFast(c2);
    for (let i = 0; i < len; i++) assert(approx(c2[i], Math.tanh(a[i]), 4e-2), "tanhFast[" + i + "]");

    /* silu(x) = x*sigmoid(x), composed from the fast-exp-based sigmoid kernel
     * (see dyna-simd.c) -- same tolerance class as sigmoid. */
    const c1b = Float32Array.from(a); silu(c1b);
    for (let i = 0; i < len; i++) assert(approx(c1b[i], a[i] / (1 + Math.exp(-a[i])), 2e-2), "silu[" + i + "]");

    /* The binding calls the silu kernel slot directly now (no sigmoid+mul
     * compose in dyna-simd.c anymore), IN PLACE (out == in). The kernel and a
     * sigmoid+mul composition are DIFFERENT fast-exp formulations (measured:
     * ~5% apart at x=-2.5, each within the 2e-2 class bound), so the pins
     * here are: kernel and composition agree within the class tolerance, and
     * in-place aliasing is bitwise the fresh-buffer result. The 2e-2 check
     * above is what catches the historical x86 sign bug. */
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
        /* per-element fast_exp error compounds through the normalization
         * ratio (measured 1.15e-2 worst-case); 3e-2 covers it with margin. */
        for (let i = 0; i < len; i++) assert(approx(c3[i], want[i] / wsum, 3e-2), "softmax[" + i + "]");

        /* logSoftmax subtracts log(sum of fast_exp): the fast-exp error rides
         * through log(sum), measured 1.83e-2 relative worst-case here; the
         * max(1,|want|)*1e-1 bound (>=5x margin) covers it. */
        const c4 = Float32Array.from(a); logSoftmax(c4);
        for (let i = 0; i < len; i++) assert(approx(c4[i], Math.log(want[i] / wsum), Math.max(1, Math.abs(Math.log(want[i] / wsum))) * 1e-1), "logSoftmax[" + i + "]");
    } else {
        let threw = false; try { softmax(Float32Array.from(a)); } catch { threw = true; }
        assert(threw, "softmax: empty array throws");
        threw = false; try { logSoftmax(Float32Array.from(a)); } catch { threw = true; }
        assert(threw, "logSoftmax: empty array throws");
    }
}

/* ---------- unary math: vexp (loose), vlog/vsqrt/vrsqrt/vinv (exact) ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 7), p = posVec(len, 8);

    const c1 = Float32Array.from(a); vexp(c1);
    /* fast_exp measured worst-case relative error 6.09e-2 over this vector
     * range (rndVec seed 7); max(1,|exp|)*1e-1 covers it with ~1.6x margin. */
    for (let i = 0; i < len; i++) assert(approx(c1[i], Math.exp(a[i]), Math.max(1, Math.abs(Math.exp(a[i]))) * 1e-1), "vexp[" + i + "]");

    /* vlog: the kernel computes logf(x) in single precision; this compares it
     * against fround(log(x)), i.e. a DOUBLE-precision log rounded down to f32.
     * Those two are not required to be bit-equal -- they differ whenever the
     * double result sits near an f32 rounding boundary (double rounding). They
     * happened to agree for this vector under macOS system libm, and stopped
     * agreeing on one element under openlibm, which is a property of the two
     * libm implementations and not a kernel defect. Assert 1 ULP of f32
     * instead, which is what a correctly-rounded logf actually guarantees. */
    const c2 = Float32Array.from(p); vlog(c2);
    for (let i = 0; i < len; i++) {
        const want = Math.fround(Math.log(p[i]));
        const ulp = Math.max(Math.abs(want) * 1.1920929e-7, 1e-45);
        assert(Math.abs(c2[i] - want) <= ulp, "vlog[" + i + "]");
    }

    const c3 = Float32Array.from(p); vsqrt(c3);
    /* vsqrt: NEON derives sqrt(x) from a vrsqrte + Newton-Raphson reciprocal
     * square root, not a native sqrt instruction -- near, not ===. */
    for (let i = 0; i < len; i++) assert(near(c3[i], Math.fround(Math.sqrt(p[i])), Math.max(1, Math.abs(Math.sqrt(p[i]))) * 1e-5), "vsqrt[" + i + "]");

    const c4 = Float32Array.from(p); vrsqrt(c4);
    for (let i = 0; i < len; i++) assert(near(c4[i], 1 / Math.sqrt(p[i]), Math.max(1, Math.abs(1 / Math.sqrt(p[i]))) * 1e-6), "vrsqrt[" + i + "]");

    const c5 = Float32Array.from(p); vinv(c5);
    /* vinv: NEON reciprocal-estimate + Newton-Raphson, not exact -- near, not ===. */
    for (let i = 0; i < len; i++) assert(near(c5[i], Math.fround(1 / p[i]), Math.max(1, Math.abs(1 / p[i])) * 1e-5), "vinv[" + i + "]");
}

/* ---------- distances: distL2, distL1, distCos, distCheb ---------- */
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

/* ---------- BLAS-2/3: gemv, gemvT, gemm ---------- */
for (const [m, k, nn] of [[1, 1, 1], [2, 3, 4], [5, 1, 7], [8, 8, 8], [16, 5, 3]]) {
    const A = rndVec(m * nn, 11);           /* m x n for gemv/gemvT */
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

/* BLAS dimension mismatches and coercion order */
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

/* ---------- clamp, threshold ---------- */
for (const len of SIZES) {
    const a = rndVec(len, 19);

    const c1 = Float32Array.from(a); clamp(c1, -2, 2);
    for (let i = 0; i < len; i++) assert(c1[i] === Math.fround(Math.min(2, Math.max(-2, a[i]))), "clamp[" + i + "]");

    const c2 = Float32Array.from(a); threshold(c2, 0);
    for (let i = 0; i < len; i++) assert(c2[i] === (a[i] > 0 ? 1 : 0), "threshold[" + i + "]");
}

/* ---------- topkIndices ---------- */
for (const [len, k] of [[0, 3], [1, 0], [1, 5], [5, 0], [5, 2], [5, 5], [5, 100], [100, 10]]) {
    const vals = rndVec(len, 20);
    const idx = topkIndices(vals, k);
    const wantK = Math.min(k, len);
    assert(idx.length === wantK, "topkIndices length len=" + len + " k=" + k);
    assert(idx instanceof Uint32Array, "topkIndices returns Uint32Array");

    const got = Array.from(idx, (i) => vals[i]).sort((x, y) => y - x);
    const want = Array.from(vals).sort((x, y) => y - x).slice(0, wantK);
    for (let i = 0; i < wantK; i++) assert(got[i] === want[i], "topkIndices value[" + i + "] len=" + len + " k=" + k);

    /* indices must be unique and in-bounds */
    const seen = new Set();
    for (const i of idx) { assert(i < len, "topkIndices index in bounds"); assert(!seen.has(i), "topkIndices index unique"); seen.add(i); }
}

/* ---------- type + shape errors (original coverage, unchanged) ---------- */
{
    let t = false; try { dot(new Float32Array(3), new Float32Array(4)); } catch { t = true; }
    assert(t, "length mismatch throws");
    t = false; try { dot(new Uint8Array(4), new Uint8Array(4)); } catch { t = true; }
    assert(t, "non-Float32Array throws");
    t = false; try { sum([1, 2, 3]); } catch { t = true; }
    assert(t, "plain array throws");
}

/* reentrancy: a scalar arg whose valueOf mutates must not corrupt (coerced first) */
{
    const a = new Float32Array([1, 2, 3, 4]);
    let calls = 0;
    scale(a, { valueOf() { calls++; return 2; } });
    assert(calls === 1 && a[0] === 2 && a[3] === 8, "scale coerces scalar first");
}

print("test_simd: all tests passed (" + n + " assertions)");
