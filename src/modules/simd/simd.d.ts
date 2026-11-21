/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:simd
/** dyna:simd — explicit vector math over typed arrays: reductions, dot products, distances, activations, matrix ops; the best kernel for the CPU is chosen at startup.
 *  Plain names are f32 (F32Like), `f64*` take Float64Array, `i32*` strictly Int32Array; most kernels accept a trailing (offset, length) window.
 *  An Int32Array or Uint32Array given to a plain (f32) kernel is a bit-cast view: its words are read as float32 bit patterns, not as integers (sum([1, 2]) is about 4e-45); integer data goes to the i32* kernels. */
declare module "dyna:simd" {
    // Reductions accumulate in vector lanes: they match a scalar loop to a relative tolerance, not bitwise; NaN propagates (argmax/argmin ignore it).

    /** Arithmetic sum of a float array. */
    function sum(a: F32Like): number;
    /** Largest element (min is the smallest); an array of infinities answers that infinity. */
    function max(a: F32Like): number;
    /** The minimum value. Throws `RangeError` on an empty array. */
    function min(a: F32Like): number;
    /** Index of the extreme; empty throws. Ties resolve to the first occurrence, identically on every backend. */
    function argmax(a: F32Like): number;
    /** The index of the minimum. Throws `RangeError` on an empty array. */
    function argmin(a: F32Like): number;
    /** The 1-norm. */
    function normL1(a: F32Like): number;
    /** The Euclidean norm. */
    function normL2(a: F32Like): number;

    /** Elementwise: out[i] = a[i] op b[i]; all lengths must match. */
    function add(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Elementwise out = a - b (mul and div likewise); returns out. */
    function sub(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] * b[i]` and returns `out`. All lengths must match. */
    function mul(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] / b[i]` and returns `out`. All lengths must match. */
    function div(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = |a[i]|` and returns `out`. */
    function abs(out: F32Like, a: F32Like): F32Like;
    /** In-place z[i] += a[i] * b[i]. */
    function fma(z: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Inner product. */
    function dot(a: F32Like, b: F32Like): number;

    /** In-place scalar ops. */
    function scale(a: F32Like, s: number): F32Like;
    /** In-place a[i] += s; returns a. */
    function addScalar(a: F32Like, s: number): F32Like;
    /** y[i] += alpha * x[i]. */
    function axpy(y: F32Like, alpha: number, x: F32Like): F32Like;
    /** a[i] = alpha * a[i] + beta. */
    function affine(a: F32Like, alpha: number, beta: number): F32Like;

    /** In-place activations using a fast exponential: SIMD and scalar agree to a relative tolerance of 1e-3 and may differ at saturation. */
    function sigmoid(a: F32Like): F32Like;
    /** In-place `max(x, 0)`; returns `a`. */
    function relu(a: F32Like): F32Like;
    /** In-place `min(max(x, 0), 6)`; returns `a`. */
    function relu6(a: F32Like): F32Like;
    /** In-place `x < 0 ? slope * x : x`; returns `a`. */
    function leakyRelu(a: F32Like, slope: number): F32Like;
    /** In-place `x < 0 ? alpha * (exp(x) - 1) : x`; returns `a`. Uses libm expf per element (exact libm results, no approximation; kept deliberately). */
    function elu(a: F32Like, alpha: number): F32Like;
    /** In-place fast tanh via the sigmoid identity; returns `a`. */
    function tanhFast(a: F32Like): F32Like;
    /** In-place Gaussian error linear unit, tanh form 0.5x(1 + tanh(sqrt(2/pi)(x + 0.044715x^3))) with tanh from libm tanhf (no faster tanh approximation; kept deliberately); returns `a`. */
    function gelu(a: F32Like): F32Like;
    /** In-place `x * sigmoid(x)`, composed from `sigmoid` + `mul` so it is correct on every ISA; returns `a`. */
    function silu(a: F32Like): F32Like;
    /** Stable max-shifted softmax in place; a NaN input answers all-NaN. */
    function softmax(a: F32Like): F32Like;
    /** Log-softmax in place; returns `a`. Throws on an empty array. */
    function logSoftmax(a: F32Like): F32Like;

    /** In-place e^x within 2 ulp on every ISA (range reduction + degree-6 polynomial); below -88 is 0, above 88.7228 is +Infinity, NaN propagates. */
    function vexp(a: F32Like): F32Like;
    /** In-place elementwise ln via libm logf (no approximation; kept deliberately); vsqrt/vrsqrt/vinv are sqrt, 1/sqrt and 1/x, clamped to 0 where undefined (x < 0, x <= 0, x == 0). */
    function vlog(a: F32Like): F32Like;
    /** In-place square root; returns `a`. */
    function vsqrt(a: F32Like): F32Like;
    /** In-place reciprocal square root; returns `a`. */
    function vrsqrt(a: F32Like): F32Like;
    /** In-place reciprocal; returns `a`. */
    function vinv(a: F32Like): F32Like;

    /** Vector-to-scalar distances over equal-length pairs. */
    function distL2(a: F32Like, b: F32Like): number;
    /** Manhattan (L1) distance. */
    function distL1(a: F32Like, b: F32Like): number;
    /** Cosine distance: 1 - cosine similarity (0 for parallel vectors, 1 for orthogonal). */
    function distCos(a: F32Like, b: F32Like): number;
    /** Chebyshev (L-infinity) distance. */
    function distCheb(a: F32Like, b: F32Like): number;

    /** BLAS-2/3 kernels are row-major with explicit dimensions. gemv, 6-argument form: y = beta*y + A*x. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** 7-argument gemv: y = alpha*A*x + beta*y (beta == 0 skips reading y). */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, alpha: number, beta: number): F32Like;
    /** Transposed gemv: y = beta*y + A^T*x. */
    function gemvT(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** General matrix multiply C = alpha*A*B + beta*C; the output must not share memory with an input. */
    function gemm(c: F32Like, a: F32Like, b: F32Like, m: number, n: number, k: number, alpha: number, beta: number): F32Like;

    // Windows: vector kernels (not gemv/gemvT/gemm/topkIndices) take a trailing (offset, length) or {offset, length|limit}: sum(a, 8, 16). An empty bag {} (or one holding only undefined/null) is the same as no window: the whole array; an offset without length (or limit) is a TypeError "window requires length (or limit)"; length and limit together are a TypeError; any other key is a TypeError naming the valid set.|limit}: sum(a, 8, 16).

    /** Arithmetic mean = sum(a)/n. */
    function mean(a: F32Like, offset?: number, length?: number): number;
    function mean(a: F32Like, opts?: { offset?: number; length?: number; limit?: number }): number;
    /** POPULATION variance (divide by n), two-pass centered; multiply by n/(n-1) for the sample form DataFrame.VARIANCE uses. */
    function variance(a: F32Like): number;
    /** In-place z-score (a[i]-mean)/std; returns a. A constant array has no z-score: the output is all-NaN or rounding noise. */
    function normalize(a: F32Like): F32Like;
    /** Arithmetic mean over Float64Array = f64Sum(a)/n. */
    function f64Mean(a: Float64Array): number;
    /** Two-pass centered population variance over Float64Array. */
    function f64Variance(a: Float64Array): number;
    /** Mean over Int32Array with exact int64 accumulation and one final rounding. */
    function i32Mean(a: Int32Array): number;

    /** In-place min(max(x, lo), hi). */
    function clamp(a: F32Like, lo: number, hi: number): F32Like;
    /** In-place binarise: x > t ? 1.0 : 0.0. */
    function threshold(a: F32Like, t: number): F32Like;
    /** Indices of the k largest values, best first (a fresh array): descending value, NaN last, equal values by lower index. */
    function topkIndices(vals: F32Like, k: number): Uint32Array;

    /** Zero-copy Float64Array kernels. */
    function f64Sum(a: Float64Array): number;
    /** The inner product. Reorders additions and rounds slightly differently from a sequential sum. */
    function f64Dot(a: Float64Array, b: Float64Array): number;
    /** The maximum; throws on an empty array. Bit-exact. */
    function f64Max(a: Float64Array): number;
    /** The minimum; throws on an empty array. Bit-exact. */
    function f64Min(a: Float64Array): number;
    /** In-place `a[i] *= s`; returns `a`. Bit-exact. */
    function f64Scale(a: Float64Array, s: number): Float64Array;
    /** In-place y += alpha*x; returns y. */
    function f64Axpy(y: Float64Array, alpha: number, x: Float64Array): Float64Array;

    /** Zero-copy Int32Array kernels only. */
    function i32Sum(a: Int32Array): number;
    /** Exact minimum; throws on an empty array. */
    function i32Min(a: Int32Array): number;
    /** Exact maximum; throws on an empty array. */
    function i32Max(a: Int32Array): number;
    /** Sum of double products; matches a sequential dot to a relative tolerance. */
    function i32Dot(a: Int32Array, b: Int32Array): number;
    /** Writes into a separate `out`, wrapping per element (mod 2³², like `(a+b)|0`); returns `out`. */
    function i32Add(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** Writes into a separate `out`, keeping the low 32 bits per element (like `Math.imul`); returns `out`. */
    function i32Mul(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** In-place low-32-bit multiply; returns `a`. */
    function i32Scale(a: Int32Array, s: number): Int32Array;

    /** Inclusive prefix scan, in place (the output type follows the input). */
    function cumsum(a: Int32Array): Int32Array;
    /** out[i] = src[idx[i]] (raw 32-bit copy; f64Gather for Float64Array); every index is checked before the first write, out may not overlap src or idx. */
    function gather(out: F32Like, src: F32Like, idx: Int32Array): F32Like;
    function f64Gather(out: Float64Array, src: Float64Array, idx: Int32Array): Float64Array;
    /** dst[idx[i]] += vals[i] in index order (Float32Array only; f64ScatterAdd for Float64Array); indices checked first, dst may not overlap idx or vals. */
    function scatterAdd(dst: Float32Array, idx: Int32Array, vals: Float32Array): Float32Array;
    function f64ScatterAdd(dst: Float64Array, idx: Int32Array, vals: Float64Array): Float64Array;
    /** dst[i] += src[idx[i]] for i in index order (Float32Array only, one float32 rounding per add; f64GatherAdd for Float64Array); dst.length === idx.length, every index is checked against src before the first write, dst may not overlap src or idx. The gather half of a sparse update (embedding-bag sums, CSR row pulls). */
    function gatherAdd(dst: Float32Array, src: Float32Array, idx: Int32Array): Float32Array;
    function f64GatherAdd(dst: Float64Array, src: Float64Array, idx: Int32Array): Float64Array;
    /** Sparse matrix-vector product y = A*x, A in dyna:ml's CSR (scipy) layout: row r holds values[k] at columns[k] for k in [rowPointers[r], rowPointers[r+1]); duplicate columns in a row sum. y.length is the row count, so rowPointers.length === y.length + 1, rowPointers[0] === 0, non-decreasing, ending at values.length === columns.length; every column is checked against x.length before y is written (RangeError otherwise). y may not overlap any input. Each row sums its products left to right in float64 and rounds once to float32 (f64Spmv: the plain float64 left-to-right sum, bit-identical to a JS loop). Overwrites y; returns y. */
    function spmv(y: Float32Array, values: Float32Array, columns: Int32Array, rowPointers: Int32Array, x: Float32Array): Float32Array;
    function f64Spmv(y: Float64Array, values: Float64Array, columns: Int32Array, rowPointers: Int32Array, x: Float64Array): Float64Array;
    /** Float32 accumulates in float32: over long inputs the running sum drifts (about 1.5% across a window 360 000 samples in), so differences of prefix sums need the Float64Array form. */
    function cumsum(a: Float32Array): Float32Array;
    /** Left-to-right float64 accumulation, in place; every element is the exactly-ordered running sum. */
    function cumsum(a: Float64Array): Float64Array;
    /** Inclusive prefix maximum, in place; returns `a`. Exact for both (max rounds nothing). */
    function cummax(a: Int32Array): Int32Array;
    function cummax(a: Float32Array): Float32Array;
}
