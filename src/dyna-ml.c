/*
 * dyna:ml -- native machine learning, self-contained and in-repo (no external
 * deps). A from-scratch replacement for the old secure-c-libs binding, with the
 * exact same JS API:
 *
 *   import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";
 *   const m = new LinearRegression();
 *   try { m.fit([[1],[2],[3]], [3,5,7]); print(m.predict([[4]])[0]); }
 *   finally { m.close(); }        // deterministic free
 *
 * Memory model (see dyna-nat.h): each model is one malloc-backed native struct
 * that owns its coefficient/centroid buffers; disposal frees them and the struct
 * (no arena, no GC tracing). JS array inputs are COPIED into short-lived,
 * contiguous C double buffers, the math runs in C, and results are COPIED back
 * into fresh JS Arrays at the boundary -- no native pointer ever escapes.
 *
 * Reentrancy discipline (critical): every method coerces ALL its JS array args
 * into C buffers FIRST, THEN resolves the native handle via dyn_res_native
 * (which throws if the model was closed), with no JS-invoking call between the
 * resolve and the native use. Reading a JS array can run user valueOf/Proxy code
 * that close()s `this`; resolving first would be a use-after-free.
 *
 * Algorithms: LinearRegression is closed-form OLS via the normal equations
 * (Gaussian elimination with partial pivoting, tiny ridge for conditioning);
 * LogisticRegression is full-batch gradient descent on the sigmoid; KMeans is
 * Lloyd's algorithm with a seeded k-means++ initialization.
 */
#include "dyna-nat.h"

/* the shared pure-C generator (src/core/dyn-prng.c). dyn_splitmix64 IS the
 * stream every seeded fit here reproduces -- same function, not an equivalent
 * one, or recorded models and oracle vectors silently change. */
#include "core/dyn-prng.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_LOGREG_LR       0.1     /* gradient-descent step size */
#define DYN_LOGREG_ITERS    3000    /* full-batch iterations */
#define DYN_KMEANS_MAX_ITER 300     /* Lloyd iterations cap */
#define DYN_TREE_MAX_DEPTH  1024    /* hard floor under the optional maxDepth */
#define DYN_ML_MAX_ITERS    100000  /* logreg/SVC iteration cap (DoS bound) */
#define DYN_ML_MAX_EM_ITERS 10000   /* GMM EM iteration cap (DoS bound) */
#define DYN_ML_MAX_TREES    100000  /* nEstimators cap (DoS bound) */
#define DYN_RIDGE           1e-9    /* diagonal load for OLS conditioning */

/* ---------- vectorised f64 primitives ---------------------------------------
 *
 * Every inner loop here runs over a CONTIGUOUS row of doubles, and every one of
 * them is written as PORTABLE C with several independent accumulators rather than
 * as a call into the shared simd.* dispatch table. That is not an omission -- it
 * is what the measurements said. Full numbers and the four variants that were
 * built and timed, in decreasing order of speed; the summary:
 *
 *   - A single-accumulator FP reduction (`s += a[j]*b[j]`) CANNOT be vectorised
 *     by the compiler, because vectorising it means reassociating it and this
 *     build does not set -ffast-math. That is the real problem to solve.
 *
 *   - Splitting it into FOUR independent accumulators solves it in the source.
 *     clang then has four parallel dependency chains and packs them into lanes:
 *     verified in the generated assembly (fsub.2d / fmla.2d / fmul.2d NEON pairs,
 *     258 vector instructions in this file) both with -mcpu=native AND in a
 *     shipped build with no -mcpu flag at all, since NEON is baseline on arm64.
 *
 *   - Doing it this way BEATS calling the equivalent kernel, everywhere it was
 *     measured: logreg.fit(500x128) 184.6 -> 129.0 ms, linreg.predict(50000x128)
 *     2.27 -> 1.50 ms, kmeans.fit(5000x128) 9.35 -> 4.30 ms. The reason is that
 *     an indirect call cannot be inlined or fused into the caller, so every
 *     kernel invocation re-establishes a loop the compiler had already merged
 *     into surrounding work -- the same effect that made a SIMD CSV field scan a
 *     regression, but here it applies at every span length, not just short ones.
 *
 *   - It is also better numerically and behaviourally: the reassociation order is
 *     fixed by the SOURCE, so it does not vary with a runtime-selected kernel's
 *     lane count.
 *
 * KNOWN LIMIT, deliberately not addressed here. On arm64 NEON is baseline, so
 * compiler output is already as wide as f64 SIMD gets and there is nothing left
 * for a runtime kernel to add. On x86-64 a portable build (the default: -mcpu is
 * only added by CONFIG_NATIVE=y) targets SSE2, 2 f64 lanes, while the shared
 * kernels dispatch at runtime to AVX2 (4 lanes) or AVX-512 (8). So on x86 the
 * kernel call could still win on long rows. That is UNVERIFIED -- this host
 * cannot run AVX2 (qemu crashes in AVX detection, Rosetta caps at SSE4.2) -- and
 * an unmeasurable platform conditional is exactly what this repo does not land.
 * The follow-up is to measure it on real x86 hardware and, only if it pays, gate
 * a kernel call on `!defined(__AVX2__) && span is long`.
 *
 * NUMERICS. Four accumulators means additions happen in a different order than a
 * sequential loop would use, and the compiler may additionally contract a
 * multiply-add into a single FMA on targets that have one (arm64 does; baseline
 * x86-64 does not). So results can differ from a strictly sequential computation,
 * and between those two kinds of target, in the last ULP or so. Consequences,
 * documented in the module header: a fitted coefficient can move by an ULP, and a KMeans
 * point almost exactly equidistant from two centroids may land in either.
 * Predictions and clusterings are equivalent, not bit-equal; every value the
 * tests pin is compared with a tolerance, never with ===.
 *
 * Building with -DDYN_ML_NO_SIMD (make CONFIG_ML_NO_SIMD=y) switches every
 * primitive below to a single-accumulator sequential loop. That build is the
 * differential ORACLE this one is checked against: tests/test_ml_oracle.js dumps
 * both and diffs them, requiring every integer label to agree exactly. */

/* Sum of a[j]*b[j] over n contiguous doubles. */
static inline double dyn_ml_dot(const double *a, const double *b, size_t n)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        s0 += a[j] * b[j];
        s1 += a[j + 1] * b[j + 1];
        s2 += a[j + 2] * b[j + 2];
        s3 += a[j + 3] * b[j + 3];
    }
#endif
    for (; j < n; j++)
        s0 += a[j] * b[j];
    return (s0 + s1) + (s2 + s3);
}

/* Sum of n contiguous doubles. */
static inline double dyn_ml_sum(const double *x, size_t n)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        s0 += x[j];
        s1 += x[j + 1];
        s2 += x[j + 2];
        s3 += x[j + 3];
    }
#endif
    for (; j < n; j++)
        s0 += x[j];
    return (s0 + s1) + (s2 + s3);
}

/* Squared L2 distance, computed DIRECTLY.
 *
 * Not as ||a||^2 - 2(a.b) + ||b||^2: that identity turns the k-way argmin into a
 * dot product, which was the first thing tried here, and it lost on both counts.
 * It cancels catastrophically for points close together and far from the origin
 * (i.e. any converged cluster, and any DBScan epsilon test), and it was SLOWER
 * than this loop once this loop had four accumulators -- kmeans.fit(5000x128)
 * 6.21 ms via the identity vs 4.26 ms direct. So the identity is gone entirely,
 * along with the precomputed-norm buffers it needed. */
static inline double dyn_ml_sqdist(const double *a, const double *b, size_t d)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, t;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= d; j += 4) {
        t = a[j] - b[j];         s0 += t * t;
        t = a[j + 1] - b[j + 1]; s1 += t * t;
        t = a[j + 2] - b[j + 2]; s2 += t * t;
        t = a[j + 3] - b[j + 3]; s3 += t * t;
    }
#endif
    for (; j < d; j++) {
        t = a[j] - b[j];
        s0 += t * t;
    }
    return (s0 + s1) + (s2 + s3);
}

/* y[j] += alpha*x[j]. Element-wise, so it has no accumulator chain to break and
 * the compiler vectorises the plain loop as written. Non-fused
 * multiply-then-add, so it is bit-exact against the same expression in C or JS.
 *
 * The loop is written TWICE behind a size test, which is not redundant: on the
 * first arm the compiler knows n < DYN_ML_SMALL and fully unrolls it instead of
 * emitting a counted loop. Worth 83.8 -> 62.9 ms on logreg.fit(2000x4), where a
 * narrow row runs this millions of times. */
#define DYN_ML_SMALL 8

/* restrict is NOT decorative here: without it the compiler emits a runtime
   overlap test and a scalar fallback path on every call. Every caller passes
   a gradient buffer and a row of X, which are separate allocations. */
static inline void dyn_ml_axpy(double *restrict y, double alpha,
                               const double *restrict x, size_t n)
{
    size_t j;
    if (n < DYN_ML_SMALL) {
        for (j = 0; j < n; j++) {
            double p = alpha * x[j];
            y[j] = y[j] + p;
        }
        return;
    }
    for (j = 0; j < n; j++) {
        double p = alpha * x[j];
        y[j] = y[j] + p;
    }
}

/* out[j] = x[j]*s. Element-wise, same reasoning as dyn_ml_axpy. */
static inline void dyn_ml_scale(double *restrict out,
                                const double *restrict x, double s, size_t n)
{
    size_t j;
    for (j = 0; j < n; j++)
        out[j] = x[j] * s;
}


/* ---------- JS <-> C marshalling -------------------------------------------
 *
 * Two ingest paths for the training matrix X and vector y:
 *
 *   1. Array (of Array | of Float64Array):  each row is COPIED into a fresh,
 *      contiguous row-major double buffer (owned = free after use). A
 *      Float64Array row is bulk-memcpy'd from its backing buffer; a plain Array
 *      row is read cell-by-cell. Nothing native escapes.
 *
 *   2. Flat Float64Array + explicit (rows, cols):  the backing double buffer is
 *      aliased ZERO-COPY (owned = 0, never freed here) -- no per-cell JS crossing
 *      at all. Valid only for the synchronous span in which no JS runs, so the
 *      alias is taken as the LAST JS-touching step and the math runs with no JS
 *      call before the buffer is done being read (see the fit/predict methods).
 *
 * y is ALWAYS copied into an owned buffer (from a Float64Array it is a memcpy),
 * so it never leaves a dangling alias while a later arg's JS reads run. Reentrancy
 * rule still holds: all arg coercion precedes resolving the native handle.
 *
 * Detection: a Float64Array is any typed array with an 8-byte element (bpe == 8);
 * a BigInt64Array/BigUint64Array shares that width and would be misread (still
 * memory-safe, bounded by the buffer) -- callers must pass a Float64Array. This
 * mirrors dyna-simd.c, which treats any 4-byte typed array as Float32Array. */

typedef struct {
    double *data;   /* rows*cols row-major doubles */
    size_t rows, cols;
    int owned;      /* 1: malloc'd (free); 0: aliases a JS ArrayBuffer */
} dyn_matrix_t;

static void dyn_matrix_free(dyn_matrix_t *mx)
{
    if (mx->owned)
        free(mx->data);
    mx->data = NULL;
}

/* Resolve a Float64Array to its backing double* and element count. Returns 0, or
 * -1 (throwing) for a non-typed-array, a non-8-byte element type, a detached
 * buffer, or an out-of-bounds view. The pointer aliases the JS buffer and is
 * valid only while no JS runs. */
static int dyn_ml_get_f64(JSContext *ctx, JSValueConst v, double **pp, size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 8) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "expected a Float64Array");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) /* detached */
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (double *)(base + off);
    *pn = len / 8;
    return 0;
}

/* Length of a JS array into *out_len, or -1 (throws TypeError) if not an array. */
static int dyn_ml_len(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    JSValue lval;
    uint32_t len;
    int ret;

    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "expected an Array");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lval))
        return -1;
    ret = JS_ToUint32(ctx, &len, lval);
    JS_FreeValue(ctx, lval);
    if (ret)
        return -1;
    *out_len = len;
    return 0;
}

/* Read `n` numbers from JS array `arr` into out[0..n). 0 on success, -1 throws. */
static int dyn_ml_read_row(JSContext *ctx, JSValueConst arr, double *out,
                           size_t n)
{
    size_t j;
    for (j = 0; j < n; j++) {
        double x;
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)j);
        if (JS_IsException(v))
            return -1;
        if (JS_ToFloat64(ctx, &x, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, v);
        out[j] = x;
    }
    return 0;
}

/* Element count of one X row (a plain Array or a Float64Array) into *out, or -1
 * (throwing). A Float64Array is not a JS Array, so JS_IsArray disambiguates. */
static int dyn_ml_row_len(JSContext *ctx, JSValueConst row, size_t *out)
{
    if (JS_IsArray(ctx, row))
        return dyn_ml_len(ctx, row, out);
    {
        double *rp;
        return dyn_ml_get_f64(ctx, row, &rp, out);
    }
}

/* Copy `cols` numbers of one X row into dst. A Float64Array row is a bulk memcpy
 * from its backing buffer (used and released with no JS in between); a plain
 * Array row is read cell-by-cell. Returns 0, or -1 (throwing). */
static int dyn_ml_read_row_generic(JSContext *ctx, JSValueConst row, double *dst,
                                    size_t cols)
{
    if (JS_IsArray(ctx, row)) {
        size_t rlen;
        if (dyn_ml_len(ctx, row, &rlen))
            return -1;
        if (rlen != cols) {
            JS_ThrowTypeError(ctx,
                "every row of X must have the same length");
            return -1;
        }
        return dyn_ml_read_row(ctx, row, dst, cols);
    }
    {
        double *rp;
        size_t rn;
        if (dyn_ml_get_f64(ctx, row, &rp, &rn))
            return -1;
        if (rn != cols) {
            JS_ThrowTypeError(ctx, "every row of X must have the same length");
            return -1;
        }
        memcpy(dst, rp, cols * sizeof(double));
        return 0;
    }
}

/* Ingest an Array-of-(Array|Float64Array) X into a fresh owned row-major buffer.
 * Sets mx->{data,rows,cols}, owned=1. -1 (throwing) on error. */
static int dyn_ml_ingest_matrix_array(JSContext *ctx, JSValueConst x,
                                      dyn_matrix_t *mx)
{
    size_t rows, cols, count, i;
    double *data;
    JSValue row0;
    int err;

    if (dyn_ml_len(ctx, x, &rows))
        return -1;
    if (rows == 0) {
        JS_ThrowTypeError(ctx, "X must have at least one row");
        return -1;
    }
    row0 = JS_GetPropertyUint32(ctx, x, 0);
    if (JS_IsException(row0))
        return -1;
    err = dyn_ml_row_len(ctx, row0, &cols);
    JS_FreeValue(ctx, row0);
    if (err)
        return -1;
    if (cols == 0) {
        JS_ThrowTypeError(ctx, "X rows must have at least one feature");
        return -1;
    }
    if (rows > SIZE_MAX / cols ||
        (count = rows * cols) > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "X is too large");
        return -1;
    }
    data = (double *)malloc(count * sizeof(double));
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, x, (uint32_t)i);
        if (JS_IsException(row)) {
            free(data);
            return -1;
        }
        err = dyn_ml_read_row_generic(ctx, row, data + i * cols, cols);
        JS_FreeValue(ctx, row);
        if (err) {
            free(data);
            return -1;
        }
    }
    mx->data = data;
    mx->rows = rows;
    mx->cols = cols;
    mx->owned = 1;
    return 0;
}

/* Ingest a flat Float64Array X as a ZERO-COPY alias with explicit (rows, cols).
 * Sets mx->{data,rows,cols}, owned=0. The alias must be the last JS-touching
 * step before the math (no JS may run while it is held). -1 (throwing) on error. */
static int dyn_ml_ingest_matrix_flat(JSContext *ctx, JSValueConst x,
                                     size_t rows, size_t cols, dyn_matrix_t *mx)
{
    double *data;
    size_t total;

    if (rows == 0 || cols == 0) {
        JS_ThrowTypeError(ctx,
            "a flat Float64Array X requires positive (rows, cols)");
        return -1;
    }
    if (rows > SIZE_MAX / cols) {
        JS_ThrowRangeError(ctx, "X is too large");
        return -1;
    }
    if (dyn_ml_get_f64(ctx, x, &data, &total)) /* throws if not Float64Array */
        return -1;
    if (total != rows * cols) {
        JS_ThrowTypeError(ctx,
            "flat Float64Array length must equal rows*cols");
        return -1;
    }
    mx->data = data;
    mx->rows = rows;
    mx->cols = cols;
    mx->owned = 0;
    return 0;
}

/* Ingest y (an Array or a Float64Array) of exactly `expect` entries into a fresh
 * OWNED buffer (from a Float64Array: a memcpy). Sets *pout (caller frees). y is
 * never aliased, so no dangling view survives a later arg's JS reads. -1 throws. */
static int dyn_ml_ingest_vector(JSContext *ctx, JSValueConst y, size_t expect,
                                double **pout)
{
    size_t n;
    double *out;

    if (JS_IsArray(ctx, y)) {
        if (dyn_ml_len(ctx, y, &n))
            return -1;
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "y length must equal the number of rows in X");
            return -1;
        }
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_ml_read_row(ctx, y, out, n)) {
            free(out);
            return -1;
        }
    } else {
        double *src;
        if (dyn_ml_get_f64(ctx, y, &src, &n)) /* throws if not Float64Array */
            return -1;
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "y length must equal the number of rows in X");
            return -1;
        }
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out, src, n * sizeof(double));
    }
    *pout = out;
    return 0;
}


/* Read a numeric vector of whatever length it happens to be. dyn_ml_ingest_vector
 * exists to check a length against X; this one is for arguments whose length IS
 * the information (a CSR's value and index arrays). */
static int dyn_ml_ingest_any_vector(JSContext *ctx, JSValueConst v,
                                    double **pout, size_t *pn)
{
    size_t n;
    double *out;

    if (JS_IsArray(ctx, v)) {
        if (dyn_ml_len(ctx, v, &n))
            return -1;
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_ml_read_row(ctx, v, out, n)) {
            free(out);
            return -1;
        }
    } else {
        double *src;
        if (dyn_ml_get_f64(ctx, v, &src, &n))
            return -1;
        out = (double *)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out, src, n * sizeof(double));
    }
    *pout = out;
    *pn = n;
    return 0;
}

/* ==================================================================== *
 *  W9.4a -- missing data: rejected at fit, handled explicitly           *
 * ==================================================================== *
 *
 * Production tables have NaNs, and until now every one of them propagated
 * SILENTLY: a NaN feature poisons a mean, a variance, a centroid, a split
 * threshold and every coefficient downstream, and the model still "fits". It
 * then returns NaN predictions -- or worse, predictions that are merely wrong,
 * because one class's statistics were NaN and lost every comparison it entered.
 *
 * So every fit now REJECTS non-finite input, from one place that all 18 fitted
 * classes route through and a nineteenth cannot forget.
 *
 * The plan called for a per-fit {nanPolicy} option. This is the same capability
 * with the policy moved to where it can be seen: `imputeMean` and `dropMissing`
 * are explicit preprocessing, alongside StandardScaler and MinMaxScaler, rather
 * than an option buried in eighteen heterogeneous constructors. Imputation
 * changes the data, and a caller should be able to point at the line where that
 * happened.
 *
 * Infinities count as missing. An infinite feature breaks exactly the
 * arithmetic a NaN breaks, and a caller who meant it can say so by dropping the
 * row.
 */

/* Reject non-finite entries in X (and y, when supervised). One pass, no
 * allocation, and it names the exact cell -- an error that says "row 4813,
 * column 7" is actionable and "NaN in input" is not. */
/* `missing_ok` is for a model that gives NaN a MEANING -- the second-order
 * objective learns a direction for it -- and it still rejects infinities,
 * because an infinity is not a missing marker: it is arithmetic that already
 * went wrong, and no split can send it anywhere useful. */
static int dyn_ml_check_finite_ex(JSContext *ctx, const dyn_matrix_t *mx,
                                  const double *y, int missing_ok)
{
    size_t rows = mx->rows, cols = mx->cols, i, j;
    const double *d = mx->data;

    for (i = 0; i < rows; i++) {
        for (j = 0; j < cols; j++) {
            if (missing_ok && isnan(d[i * cols + j]))
                continue;
            if (!isfinite(d[i * cols + j])) {
                JS_ThrowRangeError(ctx,
                    "X[%u][%u] is %s; fit rejects missing data. Use "
                    "imputeMean(X) or dropMissing(X, y) first.",
                    (unsigned)i, (unsigned)j,
                    isnan(d[i * cols + j]) ? "NaN" : "infinite");
                return -1;
            }
        }
        if (y && !isfinite(y[i])) {
            JS_ThrowRangeError(ctx,
                "y[%u] is %s; a target cannot be imputed, so drop the row. "
                "Use dropMissing(X, y).",
                (unsigned)i, isnan(y[i]) ? "NaN" : "infinite");
            return -1;
        }
    }
    return 0;
}

static int dyn_ml_check_finite(JSContext *ctx, const dyn_matrix_t *mx,
                               const double *y)
{
    return dyn_ml_check_finite_ex(ctx, mx, y, 0);
}

/* The predict-path variant: same policy, but branchless so the pass
 * vectorises (the fit variant names the exact cell, which needs a branch
 * per element -- fine on a fit, a tax on a 50000-row predict). `bad`
 * counts are integer, so there is no float reassociation to get wrong. */
static int dyn_ml_check_finite_fast(JSContext *ctx, const dyn_matrix_t *mx)
{
    size_t n = mx->rows * mx->cols, i;
    const double *d = mx->data;
    size_t bad = 0;

    for (i = 0; i < n; i++)
        bad += !(d[i] > -HUGE_VAL && d[i] < HUGE_VAL);
    if (!bad)
        return 0;
    JS_ThrowRangeError(ctx,
        "predict input contains NaN or infinite values");
    return -1;
}

/* Ingest X for a method that also takes a y vector (fit). Handles arg ordering so
 * that any zero-copy alias is taken AFTER every JS-running coercion:
 *   - Array X: read X (owned copy) FIRST, then y with expect = X.rows.
 *   - flat Float64Array X: coerce (rows, cols) and read y FIRST, then alias X.
 * `argv` are the method args; rc/cc are the indices of the rows/cols args used
 * only for the flat form. Sets *mx (caller dyn_matrix_free) and *py (caller
 * free). -1 (throwing) on error. */

/* ==================================================================== *
 *  sampleWeight (W9.4)
 *
 *  Passed as a trailing options object -- fit(X, y, {sampleWeight}) -- so it
 *  does not collide with the flat-Float64Array form's positional
 *  fit(X, y, rows, cols). The options object is recognised structurally: the
 *  first argument at index >= 2 that is an object and is neither an Array nor
 *  a TypedArray.
 *
 *  NOT EVERY ESTIMATOR SUPPORTS IT, AND THE ONES THAT DO NOT SAY SO. An
 *  option that is accepted and then ignored is the worst kind -- this module
 *  shipped exactly that defect once already, when kFold's `{folds: n}` was
 *  silently dropped because the real key was `k`, so a caller asking for 4
 *  folds got 5 and it looked like it worked. dyn_ml_reject_weights is
 *  therefore called by every fit that has no weighted form, and it throws.
 *
 *  Weights must be finite, non-negative, and not all zero. A zero weight is
 *  legal and means "ignore this row", which is the whole point; all-zero is
 *  not, because there is then nothing to fit.
 * ==================================================================== */

/* Find the options object among the trailing arguments, or JS_UNDEFINED. */
static JSValueConst dyn_ml_opts(JSContext *ctx, int argc, JSValueConst *argv,
                                int from)
{
    int i;
    for (i = from; i < argc; i++) {
        if (!JS_IsObject(argv[i]) || JS_IsArray(ctx, argv[i]))
            continue;
        {   /* a TypedArray/DataView is data, not options */
            size_t off, len, bpe;
            JSValue b = JS_GetArrayBufferView(ctx, argv[i], &off, &len, &bpe);
            if (!JS_IsException(b)) {
                JS_FreeValue(ctx, b);
                continue;
            }
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        return argv[i];
    }
    return JS_UNDEFINED;
}

/* Read {sampleWeight} if present. *pw is NULL when absent -- which is what
 * every weighted loop below branches on, so "no weights" costs a predictable
 * test against a loop-invariant pointer and nothing else. */
static int dyn_ml_ingest_weights(JSContext *ctx, int argc, JSValueConst *argv,
                                 int from, size_t rows, double **pw)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue wv;
    double *w = NULL;
    size_t i;
    double sum = 0.0;

    *pw = NULL;
    if (JS_IsUndefined(opts))
        return 0;
    wv = JS_GetPropertyStr(ctx, opts, "sampleWeight");
    if (JS_IsException(wv))
        return -1;
    if (JS_IsUndefined(wv) || JS_IsNull(wv)) {
        JS_FreeValue(ctx, wv);
        return 0;
    }
    if (dyn_ml_ingest_vector(ctx, wv, rows, &w)) {
        JS_FreeValue(ctx, wv);
        JS_ThrowTypeError(ctx, "sampleWeight must have one entry per row of X");
        return -1;
    }
    JS_FreeValue(ctx, wv);

    for (i = 0; i < rows; i++) {
        if (!isfinite(w[i]) || w[i] < 0.0) {
            const char *why = isnan(w[i]) ? "NaN"
                            : (w[i] < 0.0 ? "negative" : "infinite");
            free(w);
            JS_ThrowRangeError(ctx,
                "sampleWeight[%u] is %s; weights must be finite and non-negative",
                (unsigned)i, why);
            return -1;
        }
        sum += w[i];
    }
    if (rows && (!isfinite(sum) || sum <= 0.0)) {
        free(w);
        JS_ThrowRangeError(ctx,
            "sampleWeight sums to %s; weights must be finite and have a "
            "positive sum",
            isfinite(sum) ? "zero" : "infinity");
        return -1;
    }
    *pw = w;
    return 0;
}

/* For an estimator with no weighted form: refuse rather than ignore. */
static int dyn_ml_reject_weights(JSContext *ctx, int argc, JSValueConst *argv,
                                 int from, const char *what)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue wv;
    int present;

    if (JS_IsUndefined(opts))
        return 0;
    wv = JS_GetPropertyStr(ctx, opts, "sampleWeight");
    if (JS_IsException(wv))
        return -1;
    present = !JS_IsUndefined(wv) && !JS_IsNull(wv);
    JS_FreeValue(ctx, wv);
    if (present) {
        JS_ThrowTypeError(ctx,
            "%s has no weighted fit, so sampleWeight would be ignored. "
            "Resample the rows instead, or use an estimator that supports it: "
            "LinearRegression, LogisticRegression, every tree model, "
            "XGBRegressor, XGBClassifier, KMeans, GaussianNB, StandardScaler.",
            what);
        return -1;
    }
    return 0;
}

/* Declared up here because the X ingest -- which every fit routes through --
 * has to be able to REFUSE a sparse matrix, and one place that all of them pass
 * means a nineteenth estimator cannot forget to. */
static JSClassID dyn_csr_class_id;

static int dyn_ml_refuse_csr(JSContext *ctx, JSValueConst v)
{
    if (JS_GetClassID(v) != dyn_csr_class_id)
        return 0;
    JS_ThrowTypeError(ctx,
        "this estimator has no sparse path, so it cannot take a CSR. "
        "LinearRegression and LogisticRegression do; for anything else pass "
        "X.toDense() -- explicitly, because expanding a sparse matrix is the "
        "memory the sparse form exists to avoid.");
    return -1;
}

static int dyn_ml_ingest_Xy_ex(JSContext *ctx, JSValueConst xv, JSValueConst yv,
                               JSValueConst rows_arg, JSValueConst cols_arg,
                               dyn_matrix_t *mx, double **py, int missing_ok)
{
    if (dyn_ml_refuse_csr(ctx, xv))
        return -1;
    if (JS_IsArray(ctx, xv)) {
        if (dyn_ml_ingest_matrix_array(ctx, xv, mx))
            return -1;
        if (dyn_ml_ingest_vector(ctx, yv, mx->rows, py)) {
            dyn_matrix_free(mx);
            return -1;
        }
        if (dyn_ml_check_finite_ex(ctx, mx, *py, missing_ok)) {
            dyn_matrix_free(mx);
            free(*py);
            *py = NULL;
            return -1;
        }
        return 0;
    } else {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) ||
            JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        if (dyn_ml_ingest_vector(ctx, yv, (size_t)rows64, py))
            return -1;
        if (dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                                      (size_t)cols64, mx)) {
            free(*py);
            *py = NULL;
            return -1;
        }
        if (dyn_ml_check_finite_ex(ctx, mx, *py, missing_ok)) {
            dyn_matrix_free(mx);
            free(*py);
            *py = NULL;
            return -1;
        }
        return 0;
    }
}

static int dyn_ml_ingest_Xy(JSContext *ctx, JSValueConst xv, JSValueConst yv,
                            JSValueConst rows_arg, JSValueConst cols_arg,
                            dyn_matrix_t *mx, double **py)
{
    return dyn_ml_ingest_Xy_ex(ctx, xv, yv, rows_arg, cols_arg, mx, py, 0);
}

/* Ingest X for a method with no y (predict / kmeans.fit). For a flat
 * Float64Array, (rows, cols) come from rows_arg/cols_arg. Sets *mx (caller
 * dyn_matrix_free). -1 (throwing) on error. */
static int dyn_ml_ingest_X(JSContext *ctx, JSValueConst xv,
                           JSValueConst rows_arg, JSValueConst cols_arg,
                           dyn_matrix_t *mx)
{
    if (dyn_ml_refuse_csr(ctx, xv))
        return -1;
    if (JS_IsArray(ctx, xv))
        return dyn_ml_ingest_matrix_array(ctx, xv, mx);
    {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) ||
            JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        return dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                                         (size_t)cols64, mx);
    }
}

/* Fresh Float64Array copied from a native double buffer (never aliases it). */
static JSValue dyn_ml_f64array(JSContext *ctx, const double *v, size_t n)
{
    static const double zero_stub = 0.0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (n == 0)
        v = &zero_stub;   /* JS_NewArrayBufferCopy must not see NULL */
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v, n * sizeof(double));
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

/* A rows*cols row-major native buffer as a JS matrix, SHAPE IN = SHAPE OUT:
 * `flat` (i.e. X arrived as a flat Float64Array) yields one flat Float64Array of
 * rows*cols; otherwise an Array of `rows` Arrays, matching how X was passed.
 * Callers that received X through dyn_ml_ingest_X pass mx.owned == 0 as `flat`,
 * captured BEFORE dyn_matrix_free (which clears the pointer, not the flags). */
static JSValue dyn_ml_matrix_to_js(JSContext *ctx, const double *v, size_t rows,
                                   size_t cols, int flat)
{
    JSValue out, row;
    size_t i, j;

    if (flat)
        return dyn_ml_f64array(ctx, v, rows * cols);
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    for (i = 0; i < rows; i++) {
        row = JS_NewArray(ctx);
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        for (j = 0; j < cols; j++) {
            if (JS_SetPropertyUint32(ctx, row, (uint32_t)j,
                                     JS_NewFloat64(ctx, v[i * cols + j])) < 0) {
                JS_FreeValue(ctx, row);
                JS_FreeValue(ctx, out);
                return JS_EXCEPTION;
            }
        }
        if (JS_SetPropertyUint32(ctx, out, (uint32_t)i, row) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

/* Fresh JS Array copied from a native double buffer. */
static JSValue dyn_ml_doubles_to_js(JSContext *ctx, const double *v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewFloat64(ctx, v[i])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* Fresh JS Array copied from a native int buffer. */
static JSValue dyn_ml_ints_to_js(JSContext *ctx, const int *v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewInt32(ctx, v[i])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

/* --------------------------------------------------------------------- *
 *  introsort: quicksort with a depth limit and a heapsort escape hatch   *
 *                                                                        *
 *  libc qsort is quicksort on most targets and goes quadratic -- plus    *
 *  O(n) recursion -- on adversarial key orders, and the label/score      *
 *  arrays this module sorts are exactly the adversary's surface. Every   *
 *  caller-data sort here routes through this instead of qsort.           *
 * --------------------------------------------------------------------- */
typedef int (*dyn_ml_cmp_fn)(const void *, const void *);

static void dyn_ml_sort_swap(char *a, char *b, size_t w, char *tmp)
{
    /* constant-width branches so the moves INLINE: a runtime-width memcpy is
     * a libc call, and one per swap turns quicksort's swap volume into the
     * whole cost (measured 4x on a 200k-label sort) */
    if (w == 8) {
        uint64_t t;
        memcpy(&t, a, 8);
        memcpy(a, b, 8);
        memcpy(b, &t, 8);
    } else if (w == 16) {
        uint64_t t0, t1;
        memcpy(&t0, a, 8);
        memcpy(&t1, a + 8, 8);
        memcpy(a, b, 8);
        memcpy(a + 8, b + 8, 8);
        memcpy(b, &t0, 8);
        memcpy(b + 8, &t1, 8);
    } else {
        memcpy(tmp, a, w);
        memcpy(a, b, w);
        memcpy(b, tmp, w);
    }
}

static void dyn_ml_hsort(char *a, size_t n, size_t w, dyn_ml_cmp_fn cmp)
{
    char tmp[32];
    size_t i;
    for (i = n / 2; i > 0; i--) {
        size_t root = i - 1;
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= n)
                break;
            c = (l + 1 < n && cmp(a + (l + 1) * w, a + l * w) > 0) ? l + 1 : l;
            if (cmp(a + root * w, a + c * w) >= 0)
                break;
            dyn_ml_sort_swap(a + root * w, a + c * w, w, tmp);
            root = c;
        }
    }
    for (i = n; i > 1; i--) {
        size_t root = 0, end = i - 1;
        dyn_ml_sort_swap(a, a + end * w, w, tmp);
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= end)
                break;
            c = (l + 1 < end && cmp(a + (l + 1) * w, a + l * w) > 0) ? l + 1 : l;
            if (cmp(a + root * w, a + c * w) >= 0)
                break;
            dyn_ml_sort_swap(a + root * w, a + c * w, w, tmp);
            root = c;
        }
    }
}

static void dyn_ml_isort_r(char *a, size_t n, size_t w, dyn_ml_cmp_fn cmp,
                           size_t depth, size_t limit)
{
    char tmp[32];
    size_t i, j;
    while (n > 16) {
        if (depth >= limit) {          /* the killer escape: heapsort is nlogn */
            dyn_ml_hsort(a, n, w, cmp);
            return;
        }
        {                              /* median of three into a[0] */
            char *x = a, *y = a + (n / 2) * w, *z = a + (n - 1) * w;
            if (cmp(y, x) < 0) dyn_ml_sort_swap(x, y, w, tmp);
            if (cmp(z, y) < 0) dyn_ml_sort_swap(y, z, w, tmp);
            if (cmp(y, x) < 0) dyn_ml_sort_swap(x, y, w, tmp);
            dyn_ml_sort_swap(a, y, w, tmp);
        }
        {                              /* two-way partition; balanced on ties */
            size_t lo = 1, hi = n;
            for (;;) {
                while (lo < n && cmp(a + lo * w, a) < 0) lo++;
                do { hi--; } while (cmp(a + hi * w, a) > 0);
                if (lo >= hi)
                    break;
                dyn_ml_sort_swap(a + lo * w, a + hi * w, w, tmp);
                lo++;
            }
            dyn_ml_sort_swap(a, a + hi * w, w, tmp);
            /* loop on the larger half, recurse the smaller: depth stays
             * logarithmic even without the limit */
            if (hi > n - 1 - hi) {
                dyn_ml_isort_r(a + (hi + 1) * w, n - 1 - hi, w, cmp,
                               depth + 1, limit);
                n = hi;
            } else {
                dyn_ml_isort_r(a, hi, w, cmp, depth + 1, limit);
                a += (hi + 1) * w;
                n = n - 1 - hi;
            }
            depth++;
        }
    }
    for (i = 1; i < n; i++) {          /* insertion-sort tail */
        memcpy(tmp, a + i * w, w);
        for (j = i; j > 0 && cmp(tmp, a + (j - 1) * w) < 0; j--)
            memcpy(a + j * w, a + (j - 1) * w, w);
        memcpy(a + j * w, tmp, w);
    }
}

static void dyn_ml_isort(void *base, size_t n, size_t w, dyn_ml_cmp_fn cmp)
{
    size_t limit = 0, t;
    if (n < 2)
        return;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_isort_r((char *)base, n, w, cmp, 0, 2 * limit + 1);
}

static int dyn_ml_dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Distinct values of y[0..rows) into a fresh ascending array. Returns the count,
 * or 0 on OOM (throwing). Hybrid: the sorted-insert scan is O(rows * classes)
 * and wins for the usual few-class fit; past a class budget it bails to a sort
 * of the remainder plus a merge, so all-distinct adversarial labels are
 * O(rows log rows) instead of the O(rows^2) the bare scan would be. */
static size_t dyn_ml_classes(JSContext *ctx, const double *y, size_t rows,
                             double **out)
{
    double *c = (double *)malloc((rows ? rows : 1) * sizeof(double));
    size_t n = 0, i, j;

    if (!c) {
        JS_ThrowOutOfMemory(ctx);
        return 0;
    }
    for (i = 0; i < rows; i++) {
        for (j = 0; j < n; j++)
            if (c[j] == y[i])
                break;
        if (j < n)
            continue;
        for (j = n; j > 0 && c[j - 1] > y[i]; j--)
            c[j] = c[j - 1];
        c[j] = y[i];
        n++;
        if (n == 48 && i + 1 < rows) {
            size_t r = rows - i - 1, a = 0, b = 0, m = 0;
            double *rest = (double *)malloc(r * sizeof(double));
            double *merged = rest ? (double *)malloc(rows * sizeof(double)) : NULL;
            if (!rest || !merged) {
                free(rest); free(merged); free(c);
                JS_ThrowOutOfMemory(ctx);
                return 0;
            }
            memcpy(rest, y + i + 1, r * sizeof(double));
            dyn_ml_isort(rest, r, sizeof(double), dyn_ml_dbl_cmp);
            while (a < n && b < r) {
                if (c[a] < rest[b])
                    merged[m++] = c[a++];
                else if (rest[b] < c[a])
                    merged[m++] = rest[b++];
                else {                    /* equal: one copy, both advance */
                    merged[m++] = c[a++];
                    b++;
                }
            }
            while (a < n)
                merged[m++] = c[a++];
            while (b < r)
                merged[m++] = rest[b++];
            free(rest);
            free(c);
            *out = merged;
            return m;
        }
    }
    *out = c;
    return n;
}

/* ---------- linear algebra: solve (A w = b) in place, A is p*p row-major ----- */

/* Gaussian elimination with partial pivoting. On return b holds the solution.
 * Returns 0, or -1 if the system is singular (near-zero pivot). */
static int dyn_solve(double *A, double *b, size_t p)
{
    size_t col, r, c, pivot;

    for (col = 0; col < p; col++) {
        double maxv = fabs(A[col * p + col]);
        pivot = col;
        for (r = col + 1; r < p; r++) {
            double v = fabs(A[r * p + col]);
            if (v > maxv) {
                maxv = v;
                pivot = r;
            }
        }
        if (maxv < 1e-300)
            return -1;
        if (pivot != col) {
            for (c = 0; c < p; c++) {
                double t = A[col * p + c];
                A[col * p + c] = A[pivot * p + c];
                A[pivot * p + c] = t;
            }
            double tb = b[col];
            b[col] = b[pivot];
            b[pivot] = tb;
        }
        for (r = col + 1; r < p; r++) {
            double f = A[r * p + col] / A[col * p + col];
            for (c = col; c < p; c++)
                A[r * p + c] -= f * A[col * p + c];
            b[r] -= f * b[col];
        }
    }
    for (r = p; r-- > 0;) {
        double s = b[r];
        for (c = r + 1; c < p; c++)
            s -= A[r * p + c] * b[c];
        b[r] = s / A[r * p + r];
    }
    return 0;
}

/* ---------- class CSR: compressed sparse rows -------------------------------
 *
 * the sparse-matrix gap: every fit took a dense matrix, so data
 * that is 99% zeros -- one-hot categories, bag-of-words, interaction terms --
 * had to be MATERIALISED dense before it could be fitted. A 100k x 50k one-hot
 * design is 40 GB dense and 40 MB sparse, so the limit was not speed.
 *
 * A CSR is a VALUE HANDLE: constructed with the data, immutable afterwards.
 * `ptr[i]..ptr[i+1]` indexes the nonzeros of row i in `val`/`col`.
 *
 * THE ESTIMATORS THAT TAKE IT ARE THE ONES WITH A SPARSE PATH. LinearRegression
 * accumulates its normal equations over nonzero PAIRS, which is O(sum nnz_i^2)
 * against O(rows * cols^2); LogisticRegression's dot products and gradient
 * updates run over nonzeros, which is O(nnz) per iteration against
 * O(rows * cols). Every other fit REFUSES a CSR and names `.toDense()`.
 *
 * Refusing is the honest answer rather than densifying behind the caller's
 * back: expanding it silently is the 40 GB the sparse form existed to avoid,
 * and the caller should be able to point at the line where that happened. It is
 * the same rule W9.4 applied to imputation. */

typedef struct {
    double *val;        /* nnz values, row-major within each row */
    uint32_t *col;      /* nnz column indices */
    size_t *ptr;        /* rows + 1 row boundaries */
    size_t rows, cols, nnz;
} dyn_csr_t;

static void dyn_csr_dispose(void *native)
{
    dyn_csr_t *s = (dyn_csr_t *)native;
    if (s) {
        free(s->val);
        free(s->col);
        free(s->ptr);
        free(s);
    }
}

static const JSClassDef dyn_csr_class = {
    "CSR",
    .finalizer = dyn_res_finalizer,
};

/* Dot product of a sparse row with a dense vector. */
static double dyn_csr_dot(const double *val, const uint32_t *col, size_t nz,
                          const double *dense)
{
    double s = 0.0;
    size_t k;
    for (k = 0; k < nz; k++)
        s += val[k] * dense[col[k]];
    return s;
}

/* dense[col[k]] += a * val[k] */
static void dyn_csr_axpy(double *dense, double a, const double *val,
                         const uint32_t *col, size_t nz)
{
    size_t k;
    for (k = 0; k < nz; k++)
        dense[col[k]] += a * val[k];
}

/* new CSR(values, columns, rowPointers, cols).
 *
 * Every field is validated, because a malformed index is an out-of-bounds
 * write into somebody else's coefficient vector rather than a wrong answer:
 * pointers must be non-decreasing, start at 0, end at nnz, and every column
 * index must be inside `cols`. */
static JSValue dyn_csr_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    dyn_csr_t *s;
    double *val = NULL, *colf = NULL, *ptrf = NULL;
    int64_t cols64 = 0;
    size_t nnz, ncol, nptr, i;

    (void)new_target;
    if (argc < 4)
        return JS_ThrowTypeError(ctx,
            "new CSR(values, columns, rowPointers, cols) requires four arguments");
    if (dyn_ml_ingest_any_vector(ctx, argv[0], &val, &nnz))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_any_vector(ctx, argv[1], &colf, &ncol)) {
        free(val);
        return JS_EXCEPTION;
    }
    if (ncol != nnz) {
        free(val); free(colf);
        return JS_ThrowTypeError(ctx,
            "columns has %u entries but values has %u; there is one column "
            "index per value", (unsigned)ncol, (unsigned)nnz);
    }
    if (dyn_ml_ingest_any_vector(ctx, argv[2], &ptrf, &nptr)) {
        free(val); free(colf);
        return JS_EXCEPTION;
    }
    if (JS_ToInt64(ctx, &cols64, argv[3])) {
        free(val); free(colf); free(ptrf);
        return JS_EXCEPTION;
    }
    if (cols64 <= 0 || nptr < 1) {
        free(val); free(colf); free(ptrf);
        return JS_ThrowRangeError(ctx,
            "cols must be positive and rowPointers must have at least one entry");
    }
    s = (dyn_csr_t *)calloc(1, sizeof(*s));
    if (!s) {
        free(val); free(colf); free(ptrf);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->rows = nptr - 1;
    s->cols = (size_t)cols64;
    s->nnz = nnz;
    s->val = val;
    s->col = (uint32_t *)malloc((nnz ? nnz : 1) * sizeof(uint32_t));
    s->ptr = (size_t *)malloc(nptr * sizeof(size_t));
    if (!s->col || !s->ptr) {
        free(colf); free(ptrf);
        dyn_csr_dispose(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < nnz; i++) {
        if (!(colf[i] >= 0.0) || colf[i] >= (double)s->cols ||
            colf[i] != (double)(uint32_t)colf[i] || !isfinite(val[i])) {
            /* The width is read into a local BEFORE the dispose: reading
             * s->cols in the argument list of the throw is a use-after-free,
             * and it printed "[0, 0)" rather than crashing, which is how it
             * was noticed. */
            unsigned ncols = (unsigned)s->cols;
            free(colf); free(ptrf);
            dyn_csr_dispose(s);
            return JS_ThrowRangeError(ctx,
                "columns[%u] must be an integer in [0, %u) and its value finite",
                (unsigned)i, ncols);
        }
        s->col[i] = (uint32_t)colf[i];
    }
    for (i = 0; i < nptr; i++) {
        double v = ptrf[i];
        if (!(v >= 0.0) || v > (double)nnz || v != (double)(size_t)v ||
            (i > 0 && (size_t)v < s->ptr[i - 1])) {
            free(colf); free(ptrf);
            dyn_csr_dispose(s);
            return JS_ThrowRangeError(ctx,
                "rowPointers must be non-decreasing integers in [0, nnz]; "
                "entry %u is not", (unsigned)i);
        }
        s->ptr[i] = (size_t)v;
    }
    if (s->ptr[0] != 0 || s->ptr[nptr - 1] != nnz) {
        free(colf); free(ptrf);
        dyn_csr_dispose(s);
        return JS_ThrowRangeError(ctx,
            "rowPointers must start at 0 and end at the value count (%u)",
            (unsigned)nnz);
    }
    free(colf);
    free(ptrf);
    return dyn_res_wrap(ctx, dyn_csr_class_id, s, dyn_csr_dispose);
}

/* CSR.fromDense(X [, rows, cols]) -- drops exact zeros. */
static JSValue dyn_csr_from_dense(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_matrix_t mx = {0};
    dyn_csr_t *s;
    size_t i, j, nnz = 0, at = 0;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    (void)this_val;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    for (i = 0; i < mx.rows * mx.cols; i++)
        if (mx.data[i] != 0.0)
            nnz++;
    s = (dyn_csr_t *)calloc(1, sizeof(*s));
    if (!s) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->rows = mx.rows;
    s->cols = mx.cols;
    s->nnz = nnz;
    s->val = (double *)malloc((nnz ? nnz : 1) * sizeof(double));
    s->col = (uint32_t *)malloc((nnz ? nnz : 1) * sizeof(uint32_t));
    s->ptr = (size_t *)malloc((mx.rows + 1) * sizeof(size_t));
    if (!s->val || !s->col || !s->ptr) {
        dyn_matrix_free(&mx);
        dyn_csr_dispose(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < mx.rows; i++) {
        s->ptr[i] = at;
        for (j = 0; j < mx.cols; j++) {
            double v = mx.data[i * mx.cols + j];
            if (v != 0.0) {
                s->val[at] = v;
                s->col[at] = (uint32_t)j;
                at++;
            }
        }
    }
    s->ptr[mx.rows] = at;
    dyn_matrix_free(&mx);
    return dyn_res_wrap(ctx, dyn_csr_class_id, s, dyn_csr_dispose);
}

static JSValue dyn_csr_to_dense(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_csr_t *s = (dyn_csr_t *)dyn_res_native(ctx, this_val, dyn_csr_class_id);
    double *out;
    size_t i, k;
    JSValue r;

    (void)argc; (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->rows && s->cols > SIZE_MAX / s->rows)
        return JS_ThrowRangeError(ctx, "the dense form does not fit in memory");
    out = (double *)calloc(s->rows * s->cols ? s->rows * s->cols : 1,
                           sizeof(double));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < s->rows; i++)
        for (k = s->ptr[i]; k < s->ptr[i + 1]; k++)
            out[i * s->cols + s->col[k]] = s->val[k];
    r = dyn_ml_matrix_to_js(ctx, out, s->rows, s->cols, 0);
    free(out);
    return r;
}

static JSValue dyn_csr_row(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    dyn_csr_t *s;
    int64_t iv;
    double *out;
    size_t k;
    JSValue r;

    /* Coerce before resolving: the index coercion can run JS that closes the
     * matrix (CLAUDE.md section 8). */
    if (argc < 1 || JS_ToInt64(ctx, &iv, argv[0]))
        return JS_ThrowTypeError(ctx, "row(i) requires an index");
    s = (dyn_csr_t *)dyn_res_native(ctx, this_val, dyn_csr_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (iv < 0 || (uint64_t)iv >= (uint64_t)s->rows)
        return JS_ThrowRangeError(ctx, "row %lld is outside [0, %u)",
                                  (long long)iv, (unsigned)s->rows);
    out = (double *)calloc(s->cols ? s->cols : 1, sizeof(double));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (k = s->ptr[iv]; k < s->ptr[iv + 1]; k++)
        out[s->col[k]] = s->val[k];
    r = dyn_ml_doubles_to_js(ctx, out, s->cols);
    free(out);
    return r;
}

#define DYN_CSR_GETTER(name, expr)                                             \
static JSValue dyn_csr_##name(JSContext *ctx, JSValueConst this_val)            \
{                                                                              \
    dyn_csr_t *s = (dyn_csr_t *)dyn_res_native(ctx, this_val,                  \
                                               dyn_csr_class_id);              \
    if (!s) return JS_EXCEPTION;                                               \
    return (expr);                                                             \
}
DYN_CSR_GETTER(rows_get, JS_NewInt64(ctx, (int64_t)s->rows))
DYN_CSR_GETTER(cols_get, JS_NewInt64(ctx, (int64_t)s->cols))
DYN_CSR_GETTER(nnz_get, JS_NewInt64(ctx, (int64_t)s->nnz))
DYN_CSR_GETTER(density_get,
    JS_NewFloat64(ctx, (s->rows && s->cols)
        ? (double)s->nnz / ((double)s->rows * (double)s->cols) : 0.0))

static const JSCFunctionListEntry dyn_csr_proto[] = {
    JS_CFUNC_DEF("toDense", 0, dyn_csr_to_dense),
    JS_CFUNC_DEF("row", 1, dyn_csr_row),
    JS_CGETSET_DEF("rows", dyn_csr_rows_get, NULL),
    JS_CGETSET_DEF("cols", dyn_csr_cols_get, NULL),
    JS_CGETSET_DEF("nnz", dyn_csr_nnz_get, NULL),
    JS_CGETSET_DEF("density", dyn_csr_density_get, NULL),
};

static const JSCFunctionListEntry dyn_csr_statics[] = {
    JS_CFUNC_DEF("fromDense", 1, dyn_csr_from_dense),
};

/* Borrow a CSR argument, or NULL when the value is not one. Borrowing is safe
 * for the same reason it is safe for a Path: nothing between this call and the
 * end of the fit runs user JS. */
static dyn_csr_t *dyn_ml_as_csr(JSContext *ctx, JSValueConst v)
{
    if (JS_GetClassID(v) != dyn_csr_class_id)
        return NULL;
    return (dyn_csr_t *)dyn_res_native(ctx, v, dyn_csr_class_id);
}

/* ---------- LinearRegression: closed-form OLS (normal equations) ------------ */

typedef struct {
    int fitted;
    size_t n_features;
    double *coef;      /* n_features weights */
    double intercept;
} dyn_linreg_t;

static JSClassID dyn_linreg_class_id;

static void dyn_linreg_dispose(void *native)
{
    dyn_linreg_t *m = (dyn_linreg_t *)native;
    if (m) {
        free(m->coef);
        free(m);
    }
}

static const JSClassDef dyn_linreg_class = {
    "LinearRegression",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_linreg_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;

    (void)new_target; (void)argc; (void)argv;
    m = (dyn_linreg_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_res_wrap(ctx, dyn_linreg_class_id, m, dyn_linreg_dispose);
}

/* Solve OLS with an intercept: design column p-1 is the constant 1. Stores the
 * fitted coef/intercept into `m`. Returns 0, or -1 (throwing) on OOM/singular. */
static int dyn_linreg_solve(JSContext *ctx, dyn_linreg_t *m, const double *X,
                            const double *y, size_t rows, size_t cols,
                            const double *sw, const dyn_csr_t *S)
{
    size_t p = cols + 1;
    double *AtA, *Aty, *coef;
    size_t i, a, bcol;

    /* p*p doubles must not wrap size_t: calloc's own overflow check only
     * catches some wraps, and a wrapped-but-small size passes it and lets the
     * accumulation loops write past the buffer. cols is caller-controlled, and
     * the flat form allows cols ~ 2^31 with rows == 1. */
    if (p > (SIZE_MAX / sizeof(double)) / p) {
        JS_ThrowRangeError(ctx, "too many features for LinearRegression");
        return -1;
    }
    AtA = (double *)calloc(p * p, sizeof(double));
    Aty = (double *)calloc(p, sizeof(double));
    coef = (double *)malloc(cols * sizeof(double));
    if (!AtA || !Aty || !coef) {
        free(AtA); free(Aty); free(coef);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    /* Accumulate A^T A and A^T y where each design row is [x_i..., 1].
     *
     * Only the upper triangle is built (it is mirrored below). The design row's
     * trailing constant 1 is handled OUTSIDE the inner loop: keeping the
     * `(bcol < cols) ? xi[bcol] : 1.0` select inside it made the loop body
     * branchy over a contiguous span, which blocks both the vector kernel and
     * the compiler's own vectoriser. What remains per (row, a) is one axpy over
     * the contiguous tail xi[a..cols) plus two scalar updates. */
    /* THE LOOP IS WRITTEN TWICE, and that is the point. Weighted least squares
     * is X^T W X b = X^T W y, which is the unweighted accumulation with every
     * row's contribution scaled by w_i -- one extra multiply. Folding that in
     * as `w ? w[i] : 1.0` would put a select in the hot accumulation and make
     * the UNWEIGHTED path pay for a feature it is not using, which is exactly
     * the shape W9.4 is required not to have. So the unweighted arm below is
     * byte-for-byte what it was before weights existed. */
    if (S) {
        /* The sparse arm. A zero contributes nothing to any of these sums, so
         * the accumulation runs over nonzero PAIRS: O(sum nnz_i^2) rather than
         * O(rows * cols^2). Only the upper triangle is built, which needs the
         * column indices of a row to be ascending -- they are, because the
         * constructor and fromDense both emit them that way and a CSR is
         * immutable. */
        for (i = 0; i < S->rows; i++) {
            size_t ka, kb, k0 = S->ptr[i], k1 = S->ptr[i + 1];
            double wi = sw ? sw[i] : 1.0;
            for (ka = k0; ka < k1; ka++) {
                size_t acol = S->col[ka];
                double va = wi * S->val[ka];
                Aty[acol] += va * y[i];
                for (kb = ka; kb < k1; kb++)
                    AtA[acol * p + S->col[kb]] += va * S->val[kb];
                AtA[acol * p + cols] += va;
            }
            Aty[cols] += wi * y[i];
            AtA[cols * p + cols] += wi;
        }
    } else if (!sw) {
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            for (a = 0; a < cols; a++) {
                double va = xi[a];
                Aty[a] += va * y[i];
                /* AtA[a][a..cols) += va * xi[a..cols) */
                dyn_ml_axpy(AtA + a * p + a, va, xi + a, cols - a);
                AtA[a * p + cols] += va;      /* bcol == cols: vb == 1 */
            }
            Aty[cols] += y[i];                /* a == cols: va == 1 */
            AtA[cols * p + cols] += 1.0;
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            double wi = sw[i];
            for (a = 0; a < cols; a++) {
                double va = wi * xi[a];
                Aty[a] += va * y[i];
                dyn_ml_axpy(AtA + a * p + a, va, xi + a, cols - a);
                AtA[a * p + cols] += va;
            }
            Aty[cols] += wi * y[i];
            AtA[cols * p + cols] += wi;
        }
    }
    /* Mirror the symmetric upper triangle and add a tiny ridge for stability.
     *
     * The ridge is scaled by the MEAN weight, so that multiplying every weight
     * by a constant leaves the fit unchanged. Weighted least squares is
     * homogeneous in w -- X^T(cW)X b = X^T(cW)y has the same solution as
     * c = 1 -- but a ridge of fixed ABSOLUTE size is not: at c = 7 it would be
     * seven times weaker relative to A^T W A, and the answer would drift
     * (measured: 7e-9 on a four-row fit). Unweighted, wsum is rows and the
     * scale is exactly 1.0, so this arm is bit-identical to what it was. */
    {
        double wscale = 1.0;
        if (sw && rows) {
            double wsum = 0.0;
            for (i = 0; i < rows; i++)
                wsum += sw[i];
            wscale = wsum / (double)rows;
        }
        for (a = 0; a < p; a++) {
            for (bcol = a + 1; bcol < p; bcol++)
                AtA[bcol * p + a] = AtA[a * p + bcol];
            AtA[a * p + a] += DYN_RIDGE * wscale;
        }
    }
    if (dyn_solve(AtA, Aty, p)) {
        free(AtA); free(Aty); free(coef);
        JS_ThrowInternalError(ctx, "LinearRegression: singular system");
        return -1;
    }
    for (a = 0; a < cols; a++)
        coef[a] = Aty[a];
    free(m->coef);
    m->coef = coef;
    m->intercept = Aty[cols];
    m->n_features = cols;
    m->fitted = 1;
    free(AtA);
    free(Aty);
    return 0;
}

static JSValue dyn_linreg_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL, *sw = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    /* For a flat Float64Array X, fit(X, y, rows, cols). Guard the optional
     * shape args by argc (argv is only padded up to the declared .length). */
    /* A CSR X takes the sparse path. y and the weights are coerced FIRST --
     * a coercion can close the matrix -- and only then is its native form
     * borrowed, which is the same order every other native argument uses. */
    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        dyn_csr_t *S;
        size_t nrows;
        if (dyn_ml_ingest_any_vector(ctx, argv[1], &y, &nrows))
            return JS_EXCEPTION;
        if (dyn_ml_ingest_weights(ctx, argc, argv, 2, nrows, &sw)) {
            free(y);
            return JS_EXCEPTION;
        }
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S) { free(y); free(sw); return JS_EXCEPTION; }
        if (S->rows != nrows) {
            free(y); free(sw);
            return JS_ThrowTypeError(ctx,
                "y has %u entries but X has %u rows",
                (unsigned)nrows, (unsigned)S->rows);
        }
        m = (dyn_linreg_t *)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
        if (!m) { free(y); free(sw); return JS_EXCEPTION; }
        if (dyn_linreg_solve(ctx, m, NULL, y, S->rows, S->cols, sw, S)) {
            free(y); free(sw);
            return JS_EXCEPTION;
        }
        free(y);
        free(sw);
        return JS_DupValue(ctx, this_val);
    }
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    /* Coerce ALL args to C buffers BEFORE resolving the handle (any zero-copy
     * alias is taken last, with no JS between it and the math below). */
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_weights(ctx, argc, argv, 2, mx.rows, &sw)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    m = (dyn_linreg_t *)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y); free(sw);
        return JS_EXCEPTION;
    }
    if (dyn_linreg_solve(ctx, m, mx.data, y, mx.rows, mx.cols, sw, NULL)) {
        dyn_matrix_free(&mx); free(y); free(sw);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    free(sw);
    return JS_DupValue(ctx, this_val);
}

/* The fitted weights and intercept, as fresh Arrays (empty/0 before fit).
 * Every other fitted class exposes them; a linear model whose coefficients
 * cannot be observed cannot be verified. */
static JSValue dyn_linreg_coef_get(JSContext *ctx, JSValueConst this_val)
{
    dyn_linreg_t *m = (dyn_linreg_t *)dyn_res_native(ctx, this_val,
                                                     dyn_linreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->coef, m->n_features);
}

static JSValue dyn_linreg_intercept_get(JSContext *ctx, JSValueConst this_val)
{
    dyn_linreg_t *m = (dyn_linreg_t *)dyn_res_native(ctx, this_val,
                                                     dyn_linreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->intercept : 0.0);
}

static JSValue dyn_linreg_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_linreg_t *m;
    dyn_matrix_t mx = {0};
    double *yout = NULL;
    size_t i;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_linreg_t *)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    yout = (double *)malloc(mx.rows * sizeof(double));
    if (!yout) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < mx.rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        yout[i] = m->intercept + dyn_ml_dot(m->coef, xi, mx.cols);
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_doubles_to_js(ctx, yout, mx.rows);
    free(yout);
    return result;
}

static const JSCFunctionListEntry dyn_linreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_linreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_linreg_predict),
    JS_CGETSET_DEF("coef", dyn_linreg_coef_get, NULL),
    JS_CGETSET_DEF("intercept", dyn_linreg_intercept_get, NULL),
};

/* ---------- LogisticRegression: binary classifier (gradient descent) -------- */

/* LogisticRegression, W9.5.
 *
 * `n_out` is 1 for two classes and K for more: the binomial deviance has a
 * single degree of freedom, so a second weight vector for two classes would be
 * exactly the negation of the first and buy nothing but a slower fit. Beyond
 * two it is one vector per class (multinomial softmax).
 *
 * `n_iter`/`converged` are reported, because "it ran 3000 iterations" and "it
 * converged after 41" are different facts about the same model and a caller
 * tuning `tol` needs to see which happened. */
typedef struct {
    int fitted;
    size_t n_features;
    size_t n_classes;       /* distinct labels seen at fit */
    size_t n_out;           /* weight vectors: 1 when n_classes == 2 */
    double *classes;        /* n_classes labels, ascending */
    double *coef;           /* n_out * n_features */
    double *intercept;      /* n_out */
    /* hyper-parameters */
    double lr, l1, l2, tol;
    size_t max_iter;
    int balanced;           /* weight each class by 1/frequency */
    double C;               /* sklearn-style inverse strength, when given */
    int have_C;
    /* fit diagnostics */
    size_t n_iter;
    int converged;
} dyn_logreg_t;

static JSClassID dyn_logreg_class_id;

static void dyn_logreg_dispose(void *native)
{
    dyn_logreg_t *m = (dyn_logreg_t *)native;
    if (!m)
        return;
    free(m->coef);
    free(m->intercept);
    free(m->classes);
    free(m);
}

static const JSClassDef dyn_logreg_class = {
    "LogisticRegression",
    .finalizer = dyn_res_finalizer,
};

/* Read an optional positive-integer option. Absent/undefined leaves *out.
 * Defined with the tree models below; declared here because the linear models
 * parse options too and this file is one translation unit, not two. */
static int dyn_opt_size(JSContext *ctx, JSValueConst obj, const char *name,
                        size_t *out, size_t minimum);
static int dyn_opt_double(JSContext *ctx, JSValueConst obj, const char *name,
                          double *out, double lo, double hi);

static JSValue dyn_logreg_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_logreg_t *m;

    double lr = DYN_LOGREG_LR, tol = 1e-4, l1 = 0.0, l2 = 0.0, C = 1.0;
    size_t max_iter = DYN_LOGREG_ITERS;
    int balanced = 0, have_C = 0;
    const char *penalty = NULL;

    (void)new_target;
    /* Every option is coerced to a C local BEFORE anything is allocated. */
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        JSValue v;
        if (dyn_opt_double(ctx, o, "learningRate", &lr, 1e-12, 1e12) ||
            dyn_opt_double(ctx, o, "tol", &tol, 0.0, 1e12) ||
            dyn_opt_double(ctx, o, "l1", &l1, 0.0, 1e12) ||
            dyn_opt_double(ctx, o, "l2", &l2, 0.0, 1e12) ||
            dyn_opt_size(ctx, o, "maxIter", &max_iter, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, o, "C");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (JS_ToFloat64(ctx, &C, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            have_C = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "penalty");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            penalty = JS_ToCString(ctx, v);
            if (!penalty) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "classWeight");
        if (JS_IsException(v)) { if (penalty) JS_FreeCString(ctx, penalty); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char *cwname = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!cwname) { if (penalty) JS_FreeCString(ctx, penalty); return JS_EXCEPTION; }
            if (!strcmp(cwname, "balanced")) balanced = 1;
            else {
                JS_ThrowTypeError(ctx, "classWeight must be \"balanced\" or absent");
                JS_FreeCString(ctx, cwname);
                if (penalty) JS_FreeCString(ctx, penalty);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, cwname);
        } else {
            JS_FreeValue(ctx, v);
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    /* A hostile maxIter makes the full-batch loop run that many O(rows*cols)
     * passes on the main thread; dyn_opt_size has no upper bound. */
    if (max_iter > DYN_ML_MAX_ITERS)
        return JS_ThrowRangeError(ctx, "maxIter must be at most %u",
                                  (unsigned)DYN_ML_MAX_ITERS);
    /* `penalty` + `C` is the scikit-learn spelling; l1/l2 are the direct one.
     * C is INVERSE strength, so a large C means little regularisation -- a
     * convention worth honouring rather than silently reinterpreting. */
    if (penalty) {
        if (!strcmp(penalty, "l2")) l2 = have_C ? 1.0 / C : 1.0;
        else if (!strcmp(penalty, "l1")) l1 = have_C ? 1.0 / C : 1.0;
        else if (!strcmp(penalty, "elasticnet")) {
            l1 = l2 = (have_C ? 1.0 / C : 1.0) * 0.5;
        } else if (strcmp(penalty, "none") != 0) {
            JS_ThrowTypeError(ctx, "penalty must be \"l1\", \"l2\", "
                                   "\"elasticnet\" or \"none\"");
            JS_FreeCString(ctx, penalty);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, penalty);
    }

    m = (dyn_logreg_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->lr = lr;
    m->max_iter = max_iter;
    m->tol = tol;
    m->l1 = l1;
    m->l2 = l2;
    m->C = C;
    m->have_C = have_C;
    m->balanced = balanced;
    return dyn_res_wrap(ctx, dyn_logreg_class_id, m, dyn_logreg_dispose);
}

static inline double dyn_sigmoid(double z)
{
    if (z >= 0.0) {
        double e = exp(-z);
        return 1.0 / (1.0 + e);
    } else {
        double e = exp(z);
        return e / (1.0 + e);
    }
}

/* Full-batch gradient descent on the mean cross-entropy, with an L1/L2 penalty,
 * optional class balancing, and -- the point of W9.5 -- A CONVERGENCE CHECK.
 *
 * It used to run exactly DYN_LOGREG_ITERS = 3000 full-batch passes, every time,
 * whatever the data did. A separable two-feature problem converges in dozens;
 * paying 3000 for it is the largest available speed win in this file, and it is
 * a correctness improvement too: a caller could not tell a converged fit from
 * one that had merely stopped.
 *
 * The stopping rule is on the GRADIENT INFINITY NORM, not on the step and not
 * on the loss. The step is the gradient times the learning rate, so a rule on
 * it makes `tol` mean something different for every `learningRate` -- the first
 * version did that and never fired, because a step of lr*g with lr=0.1 stays
 * above 1e-6 long after the fit has stopped improving. The loss is a sum over
 * rows and its scale depends on the dataset. The gradient is the thing that is
 * zero at the optimum, and `max|g| < tol` means the same thing everywhere.
 *
 * On SEPARABLE data with no penalty there is nothing to converge to: the
 * likelihood has no finite maximum and the weights grow without bound, so the
 * fit correctly runs to `maxIter`. That is not a failure, and `converged`
 * reports it honestly rather than pretending.
 *
 * THE PENALTIES ARE PROXIMAL, NOT EXPLICIT, and that is a stability property
 * rather than a refinement. An explicit step `w -= lr*(g + l2*w)` diverges as
 * soon as `lr*l2 > 1`: with the default lr of 0.1, `penalty:"l2", C:0.01`
 * (l2 = 100) sends the weights to NaN in a few iterations. The first version of
 * this code did exactly that and the test caught it. The implicit step
 *
 *     v = w - lr * g_data
 *     w = softThreshold(v, lr*l1) / (1 + lr*l2)
 *
 * is the elastic-net proximal update: unconditionally stable for any penalty
 * strength, and the L1 half produces EXACT ZEROS, which is what L1 is for. A
 * subgradient `sign(w)` shrinks toward zero and never arrives.
 *
 * Neither penalty touches the intercept -- penalising it would make the fit
 * depend on where the origin happens to be.
 *
 * Returns 0, or -1 (throwing) on OOM.
 */
static int dyn_logreg_train_w(JSContext *ctx, dyn_logreg_t *m, const double *X,
                              const double *y, size_t rows, size_t cols,
                              const double *sw, const dyn_csr_t *S)
{
    double *w = NULL, *gw = NULL, *b = NULL, *gb = NULL, *classes = NULL;
    double *cw = NULL, *p = NULL;
    size_t n_classes, n_out, i, j, k, it;
    int rc = -1;
    /* The gradient is divided by the total weight, not the row count, so the
     * learning rate keeps its meaning when weights are supplied: doubling
     * every weight must not double the step. Unweighted, wsum IS rows. */
    double wsum = 0.0;
    /* sklearn's C scales the SUM loss while this loop optimises the MEAN, so
     * the sklearn spelling carries a 1/rows factor: with C given, the
     * objective is mean_loss + 1/(2*C*rows)*||w||^2 -- sklearn's C exactly
     * (verified against sklearn 1.7.2 by the differential in
     * tests/test_ml_sklearn.js). The l1/l2 fields hold the raw 1/C values. */
    double l1p, l2p;

    n_classes = dyn_ml_classes(ctx, y, rows, &classes);
    if (n_classes == 0)
        return -1;
    if (n_classes < 2) {
        free(classes);
        JS_ThrowTypeError(ctx, "LogisticRegression needs at least two classes "
                               "in y, found %u", (unsigned)n_classes);
        return -1;
    }
    n_out = (n_classes == 2) ? 1 : n_classes;
    /* rowlab is one byte per row, so a class index past 255 would wrap and
     * silently relabel rows; refuse rather than train a corrupted model. */
    if (n_classes > 256) {
        free(classes);
        JS_ThrowRangeError(ctx,
            "LogisticRegression supports at most 256 classes, found %u",
            (unsigned)n_classes);
        return -1;
    }

    if (m->have_C && rows) {
        l1p = m->l1 / (double)rows;
        l2p = m->l2 / (double)rows;
    } else {
        l1p = m->l1;
        l2p = m->l2;
    }

    uint8_t *rowlab = NULL;
    double *roww = NULL;

    w = (double *)calloc(n_out * cols, sizeof(double));
    gw = (double *)malloc(n_out * cols * sizeof(double));
    b = (double *)calloc(n_out, sizeof(double));
    gb = (double *)malloc(n_out * sizeof(double));
    cw = (double *)malloc(n_classes * sizeof(double));
    p = (double *)malloc(n_out * sizeof(double));
    rowlab = (uint8_t *)malloc(rows ? rows : 1);
    roww = (double *)malloc((rows ? rows : 1) * sizeof(double));
    if (!w || !gw || !b || !gb || !cw || !p || !rowlab || !roww) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    /* Class weights. "balanced" is n_samples / (n_classes * count), the
     * scikit-learn definition: a class half as frequent counts twice as much,
     * so a 99/1 split stops being fitted by predicting the majority. */
    for (k = 0; k < n_classes; k++)
        cw[k] = 1.0;
    if (m->balanced) {
        for (k = 0; k < n_classes; k++) {
            size_t cnt = 0;
            for (i = 0; i < rows; i++)
                if (y[i] == classes[k]) cnt++;
            cw[k] = cnt ? (double)rows / ((double)n_classes * (double)cnt) : 1.0;
        }
    }

    if (sw) {
        for (i = 0; i < rows; i++)
            wsum += sw[i];
    } else {
        wsum = (double)rows;
    }

    /* Resolve each row's class ONCE per fit, not once per (iteration, row).
     * y, classes, cw and sw are all fixed for the whole fit, so the scan below
     * was repeated max_iter times -- up to 3000 -- and it is a data-dependent
     * break, i.e. a mispredicting branch plus an FP compare chain, per row per
     * iteration. The tree code already resolves this way (dyn_tree_class_of);
     * the linear models never got it. Sample weight folds in here too, so the
     * row body is one load. */
    {
        size_t r_;
        for (r_ = 0; r_ < rows; r_++) {
            size_t lab = 0;
            for (k = 0; k < n_classes; k++)
                if (y[r_] == classes[k]) { lab = k; break; }
            rowlab[r_] = (uint8_t)lab;
            roww[r_] = sw ? cw[lab] * sw[r_] : cw[lab];
        }
    }

    for (it = 0; it < m->max_iter; it++) {
        double maxgrad = 0.0;
        for (j = 0; j < n_out * cols; j++)
            gw[j] = 0.0;
        for (k = 0; k < n_out; k++)
            gb[k] = 0.0;

        for (i = 0; i < rows; i++) {
            const double *xi = S ? NULL : X + i * cols;
            size_t label = rowlab[i];
            double weight = roww[i];
            if (S) {
                /* Same arithmetic over the nonzeros only: O(nnz) per iteration
                 * rather than O(rows * cols). A zero feature contributes
                 * nothing to a dot product and nothing to a gradient. */
                const double *sv = S->val + S->ptr[i];
                const uint32_t *sc = S->col + S->ptr[i];
                size_t nz = S->ptr[i + 1] - S->ptr[i];
                if (n_out == 1) {
                    double z = b[0] + dyn_csr_dot(sv, sc, nz, w);
                    double err = (dyn_sigmoid(z) - (label == 1 ? 1.0 : 0.0)) * weight;
                    dyn_csr_axpy(gw, err, sv, sc, nz);
                    gb[0] += err;
                } else {
                    double mx = 0.0, sum = 0.0;
                    for (k = 0; k < n_out; k++) {
                        p[k] = b[k] + dyn_csr_dot(sv, sc, nz, w + k * cols);
                        if (k == 0 || p[k] > mx) mx = p[k];
                    }
                    for (k = 0; k < n_out; k++) { p[k] = exp(p[k] - mx); sum += p[k]; }
                    for (k = 0; k < n_out; k++) {
                        double err = (p[k] / sum - (k == label ? 1.0 : 0.0)) * weight;
                        dyn_csr_axpy(gw + k * cols, err, sv, sc, nz);
                        gb[k] += err;
                    }
                }
            } else if (n_out == 1) {
                double z = b[0] + dyn_ml_dot(w, xi, cols);
                double err = (dyn_sigmoid(z) - (label == 1 ? 1.0 : 0.0)) * weight;
                dyn_ml_axpy(gw, err, xi, cols);
                gb[0] += err;
            } else {
                /* softmax, shifted by the max so a confident row cannot
                 * overflow exp() */
                double mx = 0.0, sum = 0.0;
                for (k = 0; k < n_out; k++) {
                    p[k] = b[k] + dyn_ml_dot(w + k * cols, xi, cols);
                    if (k == 0 || p[k] > mx) mx = p[k];
                }
                for (k = 0; k < n_out; k++) { p[k] = exp(p[k] - mx); sum += p[k]; }
                for (k = 0; k < n_out; k++) {
                    double err = (p[k] / sum - (k == label ? 1.0 : 0.0)) * weight;
                    dyn_ml_axpy(gw + k * cols, err, xi, cols);
                    gb[k] += err;
                }
            }
        }

        for (k = 0; k < n_out; k++) {
            for (j = 0; j < cols; j++) {
                size_t idx = k * cols + j;
                double g = gw[idx] / wsum;
                double v = w[idx] - m->lr * g, before = w[idx];
                if (l1p != 0.0) {          /* soft-threshold: exact zeros */
                    double t = m->lr * l1p;
                    v = v > t ? v - t : (v < -t ? v + t : 0.0);
                }
                if (l2p != 0.0)
                    v /= 1.0 + m->lr * l2p;
                w[idx] = v;
                /* The convergence test is on the EFFECTIVE gradient -- the step
                 * actually taken, divided by the learning rate. With no penalty
                 * that is exactly `g`; with one it is the quantity that is zero
                 * at the penalised optimum, which `g` alone is not. */
                {
                    double eff = (before - v) / m->lr;
                    if (fabs(eff) > maxgrad) maxgrad = fabs(eff);
                }
            }
            {   /* the intercept is never penalised */
                double g = gb[k] / wsum;
                b[k] -= m->lr * g;
                if (fabs(g) > maxgrad) maxgrad = fabs(g);
            }
        }
        m->n_iter = it + 1;
        if (maxgrad < m->tol) {
            m->converged = 1;
            break;
        }
    }
    if (m->n_iter >= m->max_iter && !m->converged)
        m->converged = 0;

    free(m->coef);
    free(m->intercept);
    free(m->classes);
    m->coef = w;
    m->intercept = b;
    m->classes = classes;
    m->n_classes = n_classes;
    m->n_out = n_out;
    m->n_features = cols;
    m->fitted = 1;
    w = NULL; b = NULL; classes = NULL;
    rc = 0;
done:
    free(w); free(gw); free(b); free(gb); free(cw); free(p); free(classes);
    free(rowlab); free(roww);
    return rc;
}

/* Raw scores for one row into `out` (n_out values). */
static void dyn_logreg_scores(const dyn_logreg_t *m, const double *xi,
                              size_t cols, double *out)
{
    size_t k;
    for (k = 0; k < m->n_out; k++)
        out[k] = m->intercept[k] + dyn_ml_dot(m->coef + k * cols, xi, cols);
}

static JSValue dyn_logreg_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_logreg_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL, *sw = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    /* A CSR X takes the sparse path. y and the weights are coerced FIRST --
     * a coercion can close the matrix -- and only then is its native form
     * borrowed, which is the same order every other native argument uses. */
    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        dyn_csr_t *S;
        size_t nrows;
        if (dyn_ml_ingest_any_vector(ctx, argv[1], &y, &nrows))
            return JS_EXCEPTION;
        if (dyn_ml_ingest_weights(ctx, argc, argv, 2, nrows, &sw)) {
            free(y);
            return JS_EXCEPTION;
        }
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S) { free(y); free(sw); return JS_EXCEPTION; }
        if (S->rows != nrows) {
            free(y); free(sw);
            return JS_ThrowTypeError(ctx,
                "y has %u entries but X has %u rows",
                (unsigned)nrows, (unsigned)S->rows);
        }
        m = (dyn_logreg_t *)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
        if (!m) { free(y); free(sw); return JS_EXCEPTION; }
        if (dyn_logreg_train_w(ctx, m, NULL, y, S->rows, S->cols, sw, S)) {
            free(y); free(sw);
            return JS_EXCEPTION;
        }
        free(y);
        free(sw);
        return JS_DupValue(ctx, this_val);
    }
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_weights(ctx, argc, argv, 2, mx.rows, &sw)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    m = (dyn_logreg_t *)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y); free(sw);
        return JS_EXCEPTION;
    }
    if (dyn_logreg_train_w(ctx, m, mx.data, y, mx.rows, mx.cols, sw, NULL)) {
        dyn_matrix_free(&mx); free(y); free(sw);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

/* Shared body: predict class labels (proba=0) or probabilities (proba=1).
 * For a flat Float64Array X, predict(X, rows, cols). */
static JSValue dyn_logreg_predict_impl(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int proba)
{
    dyn_logreg_t *m;
    dyn_matrix_t mx = {0};
    double *yout = NULL, *z = NULL;
    size_t rows, i, k, K;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_logreg_t *)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    K = m->n_classes;
    yout = (double *)malloc(rows * (proba ? K : 1) * sizeof(double));
    z = (double *)malloc(m->n_out * sizeof(double));
    if (!yout || !z) {
        free(yout); free(z);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        dyn_logreg_scores(m, xi, mx.cols, z);
        if (m->n_out == 1) {
            if (proba) {
                double p1 = dyn_sigmoid(z[0]);
                yout[i * K + 0] = 1.0 - p1;
                yout[i * K + 1] = p1;
            } else {
                /* sigmoid(z) > 0.5 <=> z > 0, so labels avoid a redundant exp() */
                yout[i] = m->classes[z[0] > 0.0 ? 1 : 0];
            }
        } else {
            size_t best = 0;
            for (k = 1; k < m->n_out; k++)
                if (z[k] > z[best]) best = k;
            if (!proba) {
                yout[i] = m->classes[best];
            } else {
                double mxz = z[best], sum = 0.0;
                for (k = 0; k < m->n_out; k++) { z[k] = exp(z[k] - mxz); sum += z[k]; }
                for (k = 0; k < m->n_out; k++) yout[i * K + k] = z[k] / sum;
            }
        }
    }
    free(z);
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = proba ? dyn_ml_matrix_to_js(ctx, yout, rows, K, 0)
                   : dyn_ml_doubles_to_js(ctx, yout, rows);
    free(yout);
    return result;
}

static JSValue dyn_logreg_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0);
}

static JSValue dyn_logreg_predict_proba(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 1);
}

static JSValue dyn_logreg_get_classes(JSContext *ctx, JSValueConst t)
{
    dyn_logreg_t *m = (dyn_logreg_t *)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m) return JS_EXCEPTION;
    if (!m->fitted) return JS_ThrowInternalError(ctx, "classes before fit");
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}
static JSValue dyn_logreg_get_niter(JSContext *ctx, JSValueConst t)
{
    dyn_logreg_t *m = (dyn_logreg_t *)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->n_iter);
}
static JSValue dyn_logreg_get_converged(JSContext *ctx, JSValueConst t)
{
    dyn_logreg_t *m = (dyn_logreg_t *)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m) return JS_EXCEPTION;
    return JS_NewBool(ctx, m->converged);
}
static JSValue dyn_logreg_get_coef(JSContext *ctx, JSValueConst t)
{
    dyn_logreg_t *m = (dyn_logreg_t *)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m) return JS_EXCEPTION;
    if (!m->fitted) return JS_ThrowInternalError(ctx, "coef before fit");
    /* one row per weight vector, so the shape says whether this is a binary or
     * a multinomial fit without the caller having to count classes */
    return dyn_ml_matrix_to_js(ctx, m->coef, m->n_out, m->n_features, 0);
}
static JSValue dyn_logreg_get_intercept(JSContext *ctx, JSValueConst t)
{
    dyn_logreg_t *m = (dyn_logreg_t *)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m) return JS_EXCEPTION;
    if (!m->fitted) return JS_ThrowInternalError(ctx, "intercept before fit");
    return m->n_out == 1 ? JS_NewFloat64(ctx, m->intercept[0])
                         : dyn_ml_doubles_to_js(ctx, m->intercept, m->n_out);
}

static const JSCFunctionListEntry dyn_logreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_logreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_logreg_predict),
    JS_CFUNC_DEF("predictProba", 1, dyn_logreg_predict_proba),
    JS_CGETSET_DEF("classes", dyn_logreg_get_classes, NULL),
    JS_CGETSET_DEF("coef", dyn_logreg_get_coef, NULL),
    JS_CGETSET_DEF("intercept", dyn_logreg_get_intercept, NULL),
    JS_CGETSET_DEF("nIter", dyn_logreg_get_niter, NULL),
    JS_CGETSET_DEF("converged", dyn_logreg_get_converged, NULL),
};

/* ---------- KMeans: Lloyd's algorithm with seeded k-means++ init ------------ */

typedef struct {
    int fitted;
    size_t k;
    size_t n_features;
    uint64_t seed;
    double *centroids; /* k * n_features */
    double inertia;
} dyn_kmeans_t;

static JSClassID dyn_kmeans_class_id;

static void dyn_kmeans_dispose(void *native)
{
    dyn_kmeans_t *m = (dyn_kmeans_t *)native;
    if (m) {
        free(m->centroids);
        free(m);
    }
}

static const JSClassDef dyn_kmeans_class = {
    "KMeans",
    .finalizer = dyn_res_finalizer,
};

/* new KMeans(nClusters = 8, seed = -1) -- seed < 0 uses a fixed deterministic
 * value so clustering is reproducible (matching the binding's default arg). */
static JSValue dyn_kmeans_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    size_t k = 8;
    uint64_t seed = 0x9e3779b97f4a7c15ULL;

    (void)new_target;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int32_t kv;
        if (JS_ToInt32(ctx, &kv, argv[0]))
            return JS_EXCEPTION;
        if (kv < 1)
            return JS_ThrowRangeError(ctx, "nClusters must be >= 1");
        k = (size_t)kv;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        int32_t sv;
        if (JS_ToInt32(ctx, &sv, argv[1]))
            return JS_EXCEPTION;
        if (sv >= 0)
            seed = (uint64_t)sv + 0x9e3779b97f4a7c15ULL;
    }
    m = (dyn_kmeans_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = k;
    m->seed = seed;
    return dyn_res_wrap(ctx, dyn_kmeans_class_id, m, dyn_kmeans_dispose);
}

/* Assign every point to its nearest centroid; return the summed squared
 * distance. Distances are computed directly by dyn_ml_sqdist -- see its comment
 * for why the ||c||^2 - 2(x.c) identity was tried here and removed. */
/* `w` weights the INERTIA only -- the assignment of a point to its nearest
 * centroid does not depend on how much the point counts. */
static double dyn_km_assign(const double *X, size_t rows, size_t cols,
                            const double *cent, size_t k, int *labels,
                            const double *w)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        double best = dyn_ml_sqdist(xi, cent, cols);
        int bl = 0;
        for (c = 1; c < k; c++) {
            double d = dyn_ml_sqdist(xi, cent + c * cols, cols);
            if (d < best) {
                best = d;
                bl = (int)c;
            }
        }
        if (labels)
            labels[i] = bl;
        total += w ? w[i] * best : best;
    }
    return total;
}

/* k-means++ seeding into `cent` (k*cols). `nearest` scratch holds each point's
 * squared distance to the closest chosen centroid so far. */
static void dyn_km_plusplus(const double *X, size_t rows, size_t cols,
                            size_t k, double *cent, double *nearest,
                            uint64_t *rng)
{
    size_t c, i;
    size_t first = (size_t)(dyn_splitmix64(rng) % rows);
    memcpy(cent, X + first * cols, cols * sizeof(double));
    for (i = 0; i < rows; i++)
        nearest[i] = dyn_ml_sqdist(X + i * cols, cent, cols);
    for (c = 1; c < k; c++) {
        double sum = 0.0, target;
        size_t chosen = 0;
        for (i = 0; i < rows; i++)
            sum += nearest[i];
        if (sum <= 0.0) {
            /* All remaining points coincide with chosen centroids: pick any. */
            chosen = (size_t)(dyn_splitmix64(rng) % rows);
        } else {
            target = ((double)(dyn_splitmix64(rng) >> 11) *
                      (1.0 / 9007199254740992.0)) * sum;
            for (i = 0; i < rows; i++) {
                target -= nearest[i];
                if (target <= 0.0) {
                    chosen = i;
                    break;
                }
                chosen = i;
            }
        }
        memcpy(cent + c * cols, X + chosen * cols, cols * sizeof(double));
        for (i = 0; i < rows; i++) {
            double d = dyn_ml_sqdist(X + i * cols, cent + c * cols, cols);
            if (d < nearest[i])
                nearest[i] = d;
        }
    }
}

/* Lloyd's algorithm. Stores centroids + inertia into `m`. -1 (throwing) on OOM
 * or on fewer rows than clusters. */
static int dyn_kmeans_train(JSContext *ctx, dyn_kmeans_t *m, const double *X,
                            size_t rows, size_t cols, const double *w)
{
    size_t k = m->k, i, c, it;
    double *cent = NULL, *sums = NULL, *nearest = NULL;
    int *labels = NULL, *prev = NULL;
    double *counts = NULL;    /* weight per cluster; the count when unweighted */
    double last = 0.0;        /* the loop's own assign total: inertia on break */
    uint64_t rng = m->seed;

    if (rows < k)
        return JS_ThrowRangeError(ctx,
            "KMeans needs at least nClusters rows"), -1;
    /* k ~ rows makes seeding and every Lloyd pass O(rows^2 * cols); refuse the
     * adversarial regime instead of stalling up to 300 iterations. */
    if (k > 64 && k > rows / 2)
        return JS_ThrowRangeError(ctx,
            "KMeans nClusters too close to the number of rows"), -1;
    cent = (double *)malloc(k * cols * sizeof(double));
    sums = (double *)malloc(k * cols * sizeof(double));
    nearest = (double *)malloc(rows * sizeof(double));
    labels = (int *)malloc(rows * sizeof(int));
    prev = (int *)malloc(rows * sizeof(int));
    counts = (double *)malloc(k * sizeof(double));
    if (!cent || !sums || !nearest || !labels || !prev || !counts) {
        free(cent); free(sums); free(nearest);
        free(labels); free(prev); free(counts);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    dyn_km_plusplus(X, rows, cols, k, cent, nearest, &rng);
    for (i = 0; i < rows; i++)
        prev[i] = -1;
    for (it = 0; it < DYN_KMEANS_MAX_ITER; it++) {
        int changed = 0;
        last = dyn_km_assign(X, rows, cols, cent, k, labels, w);
        for (i = 0; i < rows; i++)
            if (labels[i] != prev[i]) {
                changed = 1;
                break;
            }
        if (!changed && it > 0)
            break;
        /* Recompute centroids as the mean of assigned points: one axpy of the
         * point into its cluster's accumulator, then scale by 1/count. */
        memset(sums, 0, k * cols * sizeof(double));
        for (c = 0; c < k; c++)
            counts[c] = 0.0;
        if (w) {
            for (i = 0; i < rows; i++) {
                size_t lb = (size_t)labels[i];
                counts[lb] += w[i];
                dyn_ml_axpy(sums + lb * cols, w[i], X + i * cols, cols);
            }
        } else {
            for (i = 0; i < rows; i++) {
                size_t lb = (size_t)labels[i];
                counts[lb] += 1.0;
                dyn_ml_axpy(sums + lb * cols, 1.0, X + i * cols, cols);
            }
        }
        for (c = 0; c < k; c++) {
            /* A cluster whose members all weigh zero has no centroid to
             * compute, which is the same situation as an empty one. */
            if (counts[c] <= 0.0) {
                /* Empty cluster: reseed to a pseudo-random point. */
                size_t r = (size_t)(dyn_splitmix64(&rng) % rows);
                memcpy(cent + c * cols, X + r * cols, cols * sizeof(double));
            } else {
                dyn_ml_scale(cent + c * cols, sums + c * cols,
                             1.0 / counts[c], cols);
            }
        }
        memcpy(prev, labels, rows * sizeof(int));
    }
    /* On the converged break the last assign already saw the final centroids;
     * only max-iteration exhaustion leaves them stale. */
    m->inertia = (it == DYN_KMEANS_MAX_ITER)
                     ? dyn_km_assign(X, rows, cols, cent, k, labels, w)
                     : last;
    free(m->centroids);
    m->centroids = cent;
    m->n_features = cols;
    m->fitted = 1;
    free(sums); free(nearest);
    free(labels); free(prev); free(counts);
    return 0;
}

static JSValue dyn_kmeans_fit(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    dyn_matrix_t mx = {0};
    double *w = NULL;
    JSValueConst rows_arg, cols_arg;

    /* For a flat Float64Array X, fit(X, rows, cols). */
    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_weights(ctx, argc, argv, 1, mx.rows, &w)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    if (dyn_kmeans_train(ctx, m, mx.data, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(w);
    return JS_DupValue(ctx, this_val);
}

/* predict(X) -> Array<int> cluster labels. */
static JSValue dyn_kmeans_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_kmeans_t *m;
    dyn_matrix_t mx = {0};
    int *labels = NULL;
    size_t rows;
    JSValue result;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    /* a NaN row loses every distance comparison and lands in cluster 0;
     * predict is held to the same no-silent-missing rule as fit */
    if (dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "X has %u features, model expects %u",
            (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    labels = (int *)malloc(rows * sizeof(int));
    if (!labels) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_km_assign(mx.data, rows, mx.cols, m->centroids, m->k, labels, NULL);
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_ints_to_js(ctx, labels, rows);
    free(labels);
    return result;
}

/* Sum of squared point-to-centroid distances of the last fit (0 before fit). */
static JSValue dyn_kmeans_inertia(JSContext *ctx, JSValueConst this_val)
{
    dyn_kmeans_t *m =
        (dyn_kmeans_t *)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->inertia : 0.0);
}

static const JSCFunctionListEntry dyn_kmeans_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_kmeans_fit),
    JS_CFUNC_DEF("predict", 1, dyn_kmeans_predict),
    JS_CGETSET_DEF("inertia", dyn_kmeans_inertia, NULL),
};

/* ---------- decision trees, random forest, gradient boosting -----------------
 *
 * ONE tree implementation serves all three ensembles. A node is either a leaf
 * carrying a prediction or an internal node carrying (feature, threshold, left,
 * right); nodes live in a single flat, growable array and children are referred
 * to by INDEX, not pointer, so growing the array cannot invalidate anything and
 * the whole tree is one allocation to free.
 *
 * Splitting. For each candidate feature the sample indices are sorted by that
 * feature's value and the split point sweeps between consecutive distinct values,
 * maintaining the left/right statistics INCREMENTALLY -- one add and one subtract
 * per step. That is what makes the fit O(rows * cols * log rows) rather than the
 * O(rows^2 * cols) of recomputing each candidate's impurity from scratch. The
 * statistics needed are only sums, so the same running-total trick works for both
 * criteria:
 *
 *   REGRESSION (variance / MSE): sum and sum of squares give variance directly.
 *   CLASSIFICATION (Gini): per-class counts give the Gini index directly.
 *
 * Thresholds are the MIDPOINT between the two bracketing values, not one of the
 * values, so a test value exactly equal to a training value is not on the
 * boundary -- the standard choice, and it makes `<=` versus `<` irrelevant.
 *
 * These are the one part of dyna:ml that is NOT vectorisable and does not pretend
 * to be: the inner loop is a data-dependent gather over a permutation of the rows
 * (`X[idx[i] * cols + f]`), with a branch per element. No lane packing survives
 * that. The lever here is algorithmic (incremental statistics, sorted sweep,
 * early stopping), which is why the fit is fast anyway. Prediction is a pointer
 * chase down O(depth) nodes -- likewise inherently scalar.
 *
 * Determinism: every stochastic choice (bootstrap sample, feature subsample,
 * GBDT row subsample) comes from the seeded SplitMix64 already used by KMeans, so
 * a given seed reproduces a given forest exactly. */

#define DYN_TREE_LEAF ((uint32_t)0xffffffffu)

/* A row whose split feature is missing (NaN) descends left. Only a model that
 * accepts missing values ever sets it, and only its predict walker reads it. */
#define DYN_TNODE_MISSING_LEFT 1

typedef struct {
    uint32_t left, right;     /* child indices, or DYN_TREE_LEAF on a leaf */
    int32_t feature;          /* split feature, or -1 on a leaf */
    /* Occupies the 4-byte hole the struct already had between `feature` and
     * `threshold`, so the node stays 32 bytes and the predict walk keeps the
     * same two-nodes-per-cache-line layout. */
    int32_t flags;
    double threshold;
    double value;             /* leaf prediction (class label or mean) */
} dyn_tnode_t;

typedef struct {
    dyn_tnode_t *node;
    size_t n_nodes, cap;
    /* Parallel arrays, written during the build and read only by
     * featureImportances / predictProba / apply. Deliberately NOT fields of
     * dyn_tnode_t: predict walks the node array on every row, and widening a
     * node from 32 to 48 bytes to carry data the hot path never reads would
     * cost cache lines for nothing.
     *
     *   improve  weighted impurity decrease at this split (0 on a leaf)
     *   nsamp    training samples that reached this node
     *   proba    n_nodes * n_classes leaf class distribution (classifier only)
     */
    double *improve;
    double *nsamp;
    double *proba;
    size_t n_classes;      /* proba stride; 0 when there is no proba array */
} dyn_tree_t;

/* Shared hyper-parameters, parsed once from a JS options object. */
typedef struct {
    size_t max_depth;         /* 0 = unlimited */
    size_t min_samples_split;
    size_t min_samples_leaf;
    size_t max_features;      /* 0 = all */
    size_t max_bins;          /* 0 = exact split finding (a sort per node) */
    int classifier;
    int newton;               /* 1 = second-order (gradient/hessian) objective */
    double lambda;            /* L2 on the leaf weight */
    double alpha;             /* L1 on the leaf weight */
    double gamma;             /* minimum gain to keep a split */
    double min_child_weight;  /* minimum hessian sum in a child */
    uint64_t seed;
} dyn_tree_opts_t;

static void dyn_tree_free(dyn_tree_t *t)
{
    free(t->node);
    free(t->improve);
    free(t->nsamp);
    free(t->proba);
    t->node = NULL;
    t->improve = t->nsamp = t->proba = NULL;
    t->n_nodes = t->cap = t->n_classes = 0;
}

/* Append a node, growing geometrically, together with its parallel metadata.
 * Returns its index, or SIZE_MAX on OOM. */
static size_t dyn_tree_push(dyn_tree_t *t)
{
    if (t->n_nodes == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 32, k = t->n_classes;
        dyn_tnode_t *p = (dyn_tnode_t *)realloc(t->node, cap * sizeof(*p));
        double *im, *ns, *pr = NULL;
        if (!p)
            return SIZE_MAX;
        t->node = p;
        im = (double *)realloc(t->improve, cap * sizeof(double));
        if (!im)
            return SIZE_MAX;
        t->improve = im;
        ns = (double *)realloc(t->nsamp, cap * sizeof(double));
        if (!ns)
            return SIZE_MAX;
        t->nsamp = ns;
        if (k) {
            pr = (double *)realloc(t->proba, cap * k * sizeof(double));
            if (!pr)
                return SIZE_MAX;
            t->proba = pr;
        }
        t->cap = cap;
    }
    memset(&t->node[t->n_nodes], 0, sizeof(t->node[0]));
    t->improve[t->n_nodes] = 0.0;
    t->nsamp[t->n_nodes] = 0.0;
    if (t->n_classes)
        memset(t->proba + t->n_nodes * t->n_classes, 0,
               t->n_classes * sizeof(double));
    return t->n_nodes++;
}

/* Record a leaf's class distribution. `cnt` holds the counts of the n samples
 * that reached it, so the row sums to 1 and predictProba can average leaves
 * across trees without re-weighting. */
static void dyn_tree_set_proba(dyn_tree_t *t, size_t self, const double *cnt,
                               size_t n)
{
    size_t i, k = t->n_classes;
    double tot = 0.0;
    if (!k || n == 0)
        return;
    /* The divisor is the SUM of the counts rather than the row count, because
     * with sample weights those are different numbers and the row is a
     * distribution over the weight. Unweighted they are equal exactly -- the
     * counts are integers -- so this is bit-identical there. */
    for (i = 0; i < k; i++)
        tot += cnt[i];
    if (tot <= 0.0)
        return;
    for (i = 0; i < k; i++)
        t->proba[self * k + i] = cnt[i] / tot;
}

/* Sort `idx[0..n)` by X[idx[i]*cols + f] ascending. Insertion sort on small
 * spans, quicksort with a median-of-three pivot above that. Ties keep their
 * relative order under insertion sort, which is all the split sweep needs. */
/* (key, row) pair. Sorting these instead of bare row indices is what makes the
 * split sort cache-resident -- see dyn_tree_sort_idx below. 16 bytes, so two
 * pairs per cache line and the swap moves key and row together. */
typedef struct {
    double key;
    uint32_t row;
    uint32_t pad;
} dyn_tree_kv_t;

/* (label, index) ascending with an index tie-break: the tie order is the
 * encounter order the stratified round-robin must reproduce. */
static int dyn_ml_kv_cmp(const void *a, const void *b)
{
    const dyn_tree_kv_t *x = (const dyn_tree_kv_t *)a, *y = (const dyn_tree_kv_t *)b;
    if (x->key != y->key)
        return (x->key < y->key) ? -1 : 1;
    return (x->row > y->row) ? 1 : (x->row < y->row) ? -1 : 0;
}

/* Sort (key, row) pairs ascending by key.
 *
 * Byte-for-byte the same algorithm as the index sort this replaced -- same
 * insertion-sort cutoff, same median-of-three pivot, same partition loop. It
 * only ever branches on key comparisons, and each key is a pure function of its
 * row, so it produces the IDENTICAL permutation, including the order it leaves
 * ties in. That matters: the split sweep accumulates sum_l/cnt_left in that
 * order, and floating-point summation is order-dependent, so a different tie
 * order could shift a gain in the last bits and pick a different split. */
/* Introsort envelope: past depth 2*log2(n) a partition falls back to heapsort.
 * Every input that stays within the limit -- i.e. everything except the
 * median-of-three killer orders -- takes the identical code path and produces
 * the identical permutation; the killers get nlogn instead of quadratic time
 * and linear recursion depth. */
static void dyn_ml_kv_hsort(dyn_tree_kv_t *a, size_t n)
{
    size_t i;
    for (i = n / 2; i > 0; i--) {
        size_t root = i - 1;
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= n)
                break;
            c = (l + 1 < n && a[l + 1].key > a[l].key) ? l + 1 : l;
            if (a[root].key >= a[c].key)
                break;
            { dyn_tree_kv_t t = a[root]; a[root] = a[c]; a[c] = t; }
            root = c;
        }
    }
    for (i = n; i > 1; i--) {
        size_t root = 0, end = i - 1;
        { dyn_tree_kv_t t = a[0]; a[0] = a[end]; a[end] = t; }
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= end)
                break;
            c = (l + 1 < end && a[l + 1].key > a[l].key) ? l + 1 : l;
            if (a[root].key >= a[c].key)
                break;
            { dyn_tree_kv_t t = a[root]; a[root] = a[c]; a[c] = t; }
            root = c;
        }
    }
}

static void dyn_ml_kv_sort_r(dyn_tree_kv_t *a, size_t n, size_t depth,
                             size_t limit)
{
    size_t i, j;
    if (n < 24) {
        for (i = 1; i < n; i++) {
            dyn_tree_kv_t v = a[i];
            for (j = i; j > 0 && a[j - 1].key > v.key; j--)
                a[j] = a[j - 1];
            a[j] = v;
        }
        return;
    }
    if (depth >= limit) {
        dyn_ml_kv_hsort(a, n);
        return;
    }
    {
        double pivot;
        size_t lo = 0, hi = n - 1, mid = n / 2;
        double x = a[0].key, y = a[mid].key, z = a[n - 1].key;
        pivot = (x < y) ? ((y < z) ? y : ((x < z) ? z : x))
                        : ((x < z) ? x : ((y < z) ? z : y));
        while (lo <= hi) {
            while (a[lo].key < pivot) lo++;
            while (a[hi].key > pivot) {
                if (hi == 0) break;
                hi--;
            }
            if (lo <= hi) {
                dyn_tree_kv_t t = a[lo];
                a[lo] = a[hi];
                a[hi] = t;
                lo++;
                if (hi == 0) break;
                hi--;
            }
        }
        if (hi + 1 > 1)
            dyn_ml_kv_sort_r(a, hi + 1, depth + 1, limit);
        if (lo < n)
            dyn_ml_kv_sort_r(a + lo, n - lo, depth + 1, limit);
    }
}

static void dyn_tree_sort_kv(dyn_tree_kv_t *a, size_t n)
{
    size_t limit = 0, t;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_kv_sort_r(a, n, 0, 2 * limit + 1);
}

/* Sort idx[0..n) by feature f of X.
 *
 * The previous version sorted the bare indices and re-read the key on every
 * comparison as X[idx[j]*cols + f] -- a multiply plus a GATHERED load, with a
 * random access pattern, O(n log n) times. Materialising the keys once into a
 * contiguous (key,row) array turns every comparison into a sequential read:
 * n gathered loads instead of n log n. This routine is ~88% of tree, forest and
 * boosting training time, so it is the whole ballgame for that family. */
static void dyn_tree_sort_idx(const double *X, size_t cols, int32_t f,
                              uint32_t *idx, size_t n, dyn_tree_kv_t *kv)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t r = idx[i];
        kv[i].row = r;
        kv[i].key = X[(size_t)r * cols + f];
    }
    dyn_tree_sort_kv(kv, n);
    for (i = 0; i < n; i++)
        idx[i] = kv[i].row;
}

/* ---- histogram split finding (W9.8) -----------------------------------------
 *
 * The sort above is ~88% of tree training time and it runs once per (node,
 * feature). Binning removes it entirely: every column is sorted ONCE per fit
 * into at most `maxBins` ordered buckets, each row keeps a one-byte bin code,
 * and a node's split scan becomes one linear pass that accumulates per-bin
 * statistics plus a sweep over the bins. O(rows) per feature instead of
 * O(rows log rows), and the pass is a sequential read of a byte array rather
 * than a gathered read of doubles.
 *
 * The candidate splits ARE the exact splitter's, and so are the thresholds.
 * Each bin remembers the smallest and largest data value it holds, so a split
 * after bin b is placed at the midpoint of the largest value in the last
 * NON-EMPTY bin at or below b and the smallest in the first non-empty bin
 * above -- the same two values the sorted sweep would have bracketed, by the
 * same midpoint rule. Taking the global bin boundary instead is a whole
 * bug's worth of difference and was measured: the training partition, the
 * gains and the importances all still matched exactly, but a row the tree had
 * never seen (37% of them, under a bootstrap) routed the other way, so 24 of
 * 72 forest fits scored differently. Neighbour tracking costs one array pass
 * over the BINS and touches no row, which is why it is free.
 *
 * Bin `nbin[f]` -- one past the last real bin -- is the MISSING bin. Nothing
 * but the second-order objective puts rows there, because every other fit
 * rejects NaN before it starts (W9.4).
 *
 * Bins are limited to 255 so a code and its missing slot both fit in a byte:
 * the code matrix is rows x cols BYTES, and making it wider to allow more bins
 * would cost more memory bandwidth in the accumulation pass than the extra
 * resolution is worth. */

#define DYN_HIST_MAX_BINS 255u

typedef struct {
    /* COLUMN-major: code[f * rows + r]. The accumulation loop walks one
     * FEATURE over a node's scattered rows, so this is the layout that decides
     * how much memory it touches -- column-major reads one `rows`-byte array,
     * row-major reads one byte out of every `cols` and drags in the whole
     * rows*cols matrix to do it. Measured below. */
    uint8_t *code;       /* cols * rows bin codes; nbin[f] means "missing" */
    double *vmin, *vmax; /* cols * max_bins: value range held by each bin */
    uint32_t *nbin;      /* cols: bins actually used, 1 <= nbin[f] <= max_bins */
    size_t max_bins, rows, cols;
    int exact;           /* every column had at most max_bins distinct values */
} dyn_hist_t;

static void dyn_hist_free(dyn_hist_t *h)
{
    free(h->code);
    free(h->vmin);
    free(h->vmax);
    free(h->nbin);
    memset(h, 0, sizeof(*h));
}

/* The exact splitter's threshold rule, factored out so the two finders cannot
 * drift apart. */
static double dyn_hist_mid(double lo, double hi)
{
    double t = lo + (hi - lo) * 0.5;
    return (t >= hi) ? lo : t;      /* adjacent doubles: fall back to `lo` */
}

/* Bin every column of X. `kv` is rows-long caller scratch (the split sort's own
 * buffer, reused) and `vals` is a rows-long double buffer. Returns 0, or -1. */
static int dyn_hist_build(dyn_hist_t *h, const double *X, size_t rows,
                          size_t cols, size_t max_bins, dyn_tree_kv_t *kv,
                          double *vals, const double *w)
{
    size_t f, i, b, m, ndist, nb;

    memset(h, 0, sizeof(*h));
    if (max_bins < 2)
        max_bins = 2;
    if (max_bins > DYN_HIST_MAX_BINS)
        max_bins = DYN_HIST_MAX_BINS;
    if (rows == 0 || cols == 0 || cols > SIZE_MAX / rows)
        return -1;
    /* vmin/vmax are cols*max_bins doubles and nbin is cols uint32s; on 32-bit
     * the byte counts wrap when cols >= 2^32/(8*max_bins) (~2.1M columns at
     * the 255-bin default), undersizing the buffers the bin-edge loop writes
     * at f*max_bins offsets. */
    if (cols > (SIZE_MAX / sizeof(double)) / max_bins ||
        cols > SIZE_MAX / sizeof(uint32_t))
        return -1;
    h->rows = rows;
    h->cols = cols;
    h->max_bins = max_bins;
    h->exact = 1;
    h->code = (uint8_t *)malloc(rows * cols);
    h->vmin = (double *)malloc(cols * max_bins * sizeof(double));
    h->vmax = (double *)malloc(cols * max_bins * sizeof(double));
    h->nbin = (uint32_t *)malloc(cols * sizeof(uint32_t));
    if (!h->code || !h->vmin || !h->vmax || !h->nbin) {
        dyn_hist_free(h);
        return -1;
    }

    for (f = 0; f < cols; f++) {
        double *lo_v = h->vmin + f * max_bins, *hi_v = h->vmax + f * max_bins;
        /* Sort the finite values only. A NaN makes every comparison false, so
         * leaving it in the sort would place it arbitrarily and corrupt the
         * distinct-value scan; missing rows get their own bin instead. */
        m = 0;
        for (i = 0; i < rows; i++) {
            double v = X[i * cols + f];
            /* A zero-weight row is not part of the training set, so it does not
             * get to place a bin edge either. Without this, "weight 0 equals a
             * deleted row" holds for the exact splitter and fails for the
             * binned one -- measured, and it was the only identity that broke. */
            if (w && w[i] == 0.0)
                continue;
            if (!isnan(v)) {
                kv[m].key = v;
                kv[m].row = (uint32_t)i;
                m++;
            }
        }
        dyn_tree_sort_kv(kv, m);
        ndist = 0;
        for (i = 0; i < m; i++)
            if (ndist == 0 || kv[i].key != vals[ndist - 1])
                vals[ndist++] = kv[i].key;

        if (ndist == 0) {
            nb = 1;                       /* every value missing */
            lo_v[0] = hi_v[0] = 0.0;
        } else if (ndist <= max_bins) {
            nb = ndist;                   /* one bin per distinct value */
            for (b = 0; b < ndist; b++)
                lo_v[b] = hi_v[b] = vals[b];
        } else {
            /* Equal-frequency cuts over the DISTINCT values, so a column with a
             * heavy mode does not spend most of its bins inside it. Colliding
             * ranks leave an empty bin, which is harmless: it holds no value,
             * so the neighbour scan steps straight over it. */
            size_t start = 0;
            h->exact = 0;
            nb = max_bins;
            for (b = 0; b < nb; b++) {
                /* This arm runs only when ndist > nb, so each step advances by
                 * more than one value and no bin comes out empty -- which is
                 * what keeps vmax non-decreasing and the binary search above
                 * correct. The clamp is belt and braces, not a live case. */
                size_t end = (b + 1 == nb)
                    ? ndist
                    : (size_t)(((double)(b + 1) * (double)ndist) / (double)nb);
                if (end <= start)
                    end = start + 1;
                if (end > ndist)
                    end = ndist;
                lo_v[b] = vals[start];
                hi_v[b] = vals[end - 1];
                start = end;
            }
        }
        h->nbin[f] = (uint32_t)nb;

        for (i = 0; i < rows; i++) {
            double v = X[i * cols + f];
            size_t lo = 0, hi = nb - 1;
            if (isnan(v)) {
                h->code[f * rows + i] = (uint8_t)nb;
                continue;
            }
            /* Lowest bin whose largest value is at least v. Every v is one of
             * the values the bins were cut from, so this is exact and needs no
             * separate edge array -- the edge between bins b and b+1 is
             * dyn_hist_mid(vmax[b], vmin[b+1]) by construction. */
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (v <= hi_v[mid])
                    hi = mid;
                else
                    lo = mid + 1;
            }
            h->code[f * rows + i] = (uint8_t)lo;
        }
    }
    return 0;
}

/* The second-order leaf weight: -G/(H+lambda), with the L1 penalty applied by
 * soft-thresholding the gradient sum. A leaf whose |G| is inside the L1 band is
 * exactly zero -- which a subgradient step never reaches, and which is the
 * whole point of having alpha. */
static double dyn_newton_leaf(double G, double H, double lambda, double alpha)
{
    double g = G, d = H + lambda;
    if (alpha > 0.0) {
        if (g > alpha)
            g -= alpha;
        else if (g < -alpha)
            g += alpha;
        else
            return 0.0;
    }
    return (d > 0.0) ? -g / d : 0.0;
}

/* The structure score of a node holding gradient sum G and hessian sum H. A
 * split's gain is half the children's scores less the parent's, less gamma. */
static double dyn_newton_score(double G, double H, double lambda, double alpha)
{
    double g = fabs(G) - alpha, d = H + lambda;
    if (g < 0.0)
        g = 0.0;
    return (d > 0.0) ? g * g / d : 0.0;
}

/* Scratch shared by every recursive call, allocated once per tree. */
typedef struct {
    const double *X;
    const double *y;
    size_t cols;
    const double *classes;    /* classifier: label values, ascending */
    size_t n_classes;
    double *cnt_left;         /* n_classes */
    double *cnt_right;
    double *cnt_tot;          /* n_classes: the node's totals, during a sweep */
    const uint8_t *cls_of;    /* rows: y's class index, resolved once per fit */
    const double *w;          /* per-row sample weight, or NULL */
    uint32_t *feat;           /* cols: candidate feature permutation */
    size_t n_cand;            /* usable prefix of feat (column subsampling) */
    uint32_t *scratch;        /* rows: sort workspace for the current node */
    dyn_tree_kv_t *kv;        /* rows: (key,row) pairs for the split sort */
    uint32_t *best_order;     /* rows: scratch order for the winning feature */
    const dyn_hist_t *hist;   /* NULL = exact split finding */
    double *hbuf;             /* (max_bins + 1) * hstride bin statistics */
    uint32_t *hnext;          /* max_bins: next non-empty bin, per sweep */
    size_t hstride;
    const double *grad;       /* second-order objective: per-row gradient */
    const double *hess;       /* ... and hessian */
    dyn_tree_opts_t opt;
    uint64_t rng;
} dyn_tree_ctx_t;

/* Index of y[r] in the ascending class table.
 *
 * Resolved ONCE per fit into a byte per row, not searched per use. The linear
 * scan this replaces ran inside the innermost loop of every classifier split --
 * once per (row, feature, node) -- so it cost a pass over the class table for
 * every element the sweep touched, and grew with the number of classes. The
 * table is built from y, so it is a pure function of it and cannot go stale. */
static size_t dyn_tree_class_of(const dyn_tree_ctx_t *c, uint32_t r)
{
    return c->cls_of ? c->cls_of[r] : 0;
}

/* Leaf prediction: the majority label, or the mean target. For a classifier
 * this leaves the class counts in c->cnt_left, which dyn_tree_build turns into
 * the leaf's probability row -- the counting pass already happened, so
 * predictProba costs nothing extra at fit time. */
static double dyn_tree_leaf_value(dyn_tree_ctx_t *c, const uint32_t *idx,
                                  size_t n)
{
    size_t i;
    if (c->opt.newton) {
        /* The value that minimises the second-order expansion of the loss over
         * this leaf, which is not the mean of anything -- so this arm cannot
         * fall through to the regression one. */
        double G = 0.0, H = 0.0;
        for (i = 0; i < n; i++) {
            G += c->grad[idx[i]];
            H += c->hess[idx[i]];
        }
        return dyn_newton_leaf(G, H, c->opt.lambda, c->opt.alpha);
    }
    if (!c->opt.classifier) {
        double s = 0.0;
        if (c->w) {
            double sw = 0.0;
            for (i = 0; i < n; i++) {
                double wi = c->w[idx[i]];
                s += wi * c->y[idx[i]];
                sw += wi;
            }
            return sw > 0.0 ? s / sw : 0.0;
        }
        for (i = 0; i < n; i++)
            s += c->y[idx[i]];
        return s / (double)n;
    }
    memset(c->cnt_left, 0, c->n_classes * sizeof(double));
    if (c->w) {
        for (i = 0; i < n; i++)
            c->cnt_left[dyn_tree_class_of(c, idx[i])] += c->w[idx[i]];
    } else {
        for (i = 0; i < n; i++)
            c->cnt_left[dyn_tree_class_of(c, idx[i])] += 1.0;
    }
    {
        size_t best = 0;
        for (i = 1; i < c->n_classes; i++)
            if (c->cnt_left[i] > c->cnt_left[best])
                best = i;    /* strict >: ties go to the smaller label */
        return c->classes[best];
    }
}

/* Weighted Gini impurity of a count vector holding `tot` samples. */
static double dyn_tree_gini(const double *cnt, size_t k, double tot)
{
    double s = 0.0;
    size_t i;
    if (tot <= 0.0)
        return 0.0;
    for (i = 0; i < k; i++)
        s += cnt[i] * cnt[i];
    return tot - s / tot;      /* tot * (1 - sum p^2), pre-multiplied by tot */
}

/* Split search over binned features.
 *
 * One accumulation pass per candidate feature builds the per-bin statistics,
 * then one sweep over bins evaluates every candidate threshold. Both loops are
 * unswitched on the objective, which is invariant for the whole fit.
 *
 * Bin statistics are [count, payload...]:
 *   gini    payload = per-class counts
 *   variance payload = (sum y, sum y^2)
 *   newton  payload = (sum g, sum h)
 *
 * Returns the winning feature in *out_f (-1 when nothing beat *out_gain), the
 * threshold, the last bin on the left, and whether missing rows go left. */
static void dyn_split_hist(dyn_tree_ctx_t *c, const uint32_t *idx, size_t n,
                           size_t nf, int32_t *out_f, double *out_thr,
                           double *out_gain, uint32_t *out_bin, int *out_mleft)
{
    const dyn_hist_t *H = c->hist;
    const uint8_t *code = H->code;
    size_t stride = c->hstride, ncls = c->n_classes;
    size_t k, b, i, j, msl = c->opt.min_samples_leaf;
    double lambda = c->opt.lambda, alpha = c->opt.alpha, gam = c->opt.gamma;
    double mcw = c->opt.min_child_weight;
    int cls = c->opt.classifier, newton = c->opt.newton;
    /* With weights, slot 0 holds the WEIGHT sum and the last slot holds the row
     * count; without them slot 0 is both, so the two accumulators below read
     * the same place and minSamplesLeaf keeps meaning rows either way. */
    size_t cslot = c->w ? stride - 1 : 0;

    for (k = 0; k < nf; k++) {
        int32_t f = (int32_t)c->feat[k];
        const uint8_t *cf = code + (size_t)f * H->rows;
        size_t nb = H->nbin[f], miss = nb;
        const double *lo_v = H->vmin + (size_t)f * H->max_bins;
        const double *hi_v = H->vmax + (size_t)f * H->max_bins;
        double *hb = c->hbuf;
        double tot_c = 0.0, tot_1 = 0.0, tot_2 = 0.0, parent = 0.0;
        double tot_n = 0.0;         /* row count, which weights do not change */
        size_t lo_b = nb, hi_b = 0;  /* the bins this NODE actually touches */
        int dir;

        /* The buffer is clean on entry and is left clean on exit: only the
         * range this feature dirtied is cleared, below. Clearing (and then
         * sweeping) all `nb` bins regardless of the node made the per-feature
         * cost O(bins) rather than O(rows touched) -- which is why 255 bins
         * lost to 8. A tree's whole point is that deep nodes hold a NARROW
         * range of values, so the range collapses exactly where it matters. */
        if (newton) {
            const double *g = c->grad, *hh = c->hess;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double *s = hb + bc * stride;
                if (bc < lo_b) lo_b = bc;
                if (bc > hi_b) hi_b = bc;
                s[0] += 1.0;
                s[1] += g[r];
                s[2] += hh[r];
            }
        } else if (c->w && cls) {
            const double *tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double *s = hb + bc * stride;
                if (bc < lo_b) lo_b = bc;
                if (bc > hi_b) hi_b = bc;
                s[0] += tw[r];
                s[1 + dyn_tree_class_of(c, r)] += tw[r];
                s[cslot] += 1.0;
            }
        } else if (c->w) {
            const double *ty = c->y, *tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double *s = hb + bc * stride;
                if (bc < lo_b) lo_b = bc;
                if (bc > hi_b) hi_b = bc;
                double v = ty[r], wi = tw[r];
                s[0] += wi;
                s[1] += wi * v;
                s[2] += wi * v * v;
                s[cslot] += 1.0;
            }
        } else if (cls) {
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double *s = hb + bc * stride;
                if (bc < lo_b) lo_b = bc;
                if (bc > hi_b) hi_b = bc;
                s[0] += 1.0;
                s[1 + dyn_tree_class_of(c, r)] += 1.0;
            }
        } else {
            const double *ty = c->y;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double *s = hb + bc * stride;
                if (bc < lo_b) lo_b = bc;
                if (bc > hi_b) hi_b = bc;
                double v = ty[r];
                s[0] += 1.0;
                s[1] += v;
                s[2] += v * v;
            }
        }

        /* Totals, and the parent impurity every candidate is measured against */
        if (cls && !newton) {
            memset(c->cnt_tot, 0, ncls * sizeof(double));
            /* bins < nb only: the missing bin (== nb) is added exactly once,
             * in the explicit block below -- counting it in both places
             * inflated every gain the node computes */
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double *s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            {   /* the missing bin sits past the last real one */
                const double *s = hb + nb * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            parent = dyn_tree_gini(c->cnt_tot, ncls, tot_c);
        } else {
            /* same double-count fix as the classifier arm above */
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double *s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                tot_1 += s[1];
                tot_2 += s[2];
            }
            {
                const double *s = hb + nb * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                tot_1 += s[1];
                tot_2 += s[2];
            }
            parent = newton ? dyn_newton_score(tot_1, tot_2, lambda, alpha)
                            : tot_2 - tot_1 * tot_1 / tot_c;
        }
        if (tot_c <= 0.0)
            continue;

        /* The first non-empty bin at or above each position. Together with the
         * last non-empty bin below it, tracked as the sweep advances, this is
         * the pair of values the sorted sweep would have bracketed -- so the
         * threshold is the exact splitter's, not the global bin boundary. */
        if (lo_b <= hi_b) {
            uint32_t nxt = (uint32_t)nb;
            c->hnext[hi_b + 1] = nxt;
            for (b = hi_b + 1; b-- > lo_b; ) {
                if (hb[b * stride] > 0.0)
                    nxt = (uint32_t)b;
                c->hnext[b] = nxt;
            }
        }

        /* Two sweeps only when there is something missing to place; otherwise
         * the direction is not a choice and the second pass would re-derive
         * the identical numbers. */
        for (dir = 0; dir < 2; dir++) {
            double cl = 0.0, s1 = 0.0, s2 = 0.0, nl_cnt = 0.0;
            const double *ms = hb + miss * stride;
            size_t prev_nz = nb;              /* last non-empty bin so far */
            if (dir == 1 && ms[0] == 0.0)
                break;
            if (cls && !newton)
                memset(c->cnt_left, 0, ncls * sizeof(double));
            if (dir == 1) {          /* dir 0 leaves the missing rows on the right */
                cl = ms[0];
                nl_cnt = ms[cslot];
                if (cls && !newton) {
                    for (j = 0; j < ncls; j++)
                        c->cnt_left[j] += ms[1 + j];
                } else {
                    s1 = ms[1];
                    s2 = ms[2];
                }
            }
            /* `b` is the NUMBER of real bins already on the left, so it runs to
             * nb inclusive. The two extra positions the obvious loop misses are
             * exactly the ones that matter when a value can be missing:
             * b == nb sends every present row left and the missing rows right,
             * and b == 0 in the other direction sends every present row right
             * and the missing rows left. Those are the splits that say
             * "missingness is itself the signal" -- and without them a column
             * whose absence is perfectly informative looks worthless. Neither
             * can fire when nothing is missing, because one side is then empty
             * and the leaf-size test rejects it. */
            /* `b` is the NUMBER of real bins on the left. Every position at or
             * below lo_b holds the same (empty) left set, and every position
             * above hi_b+1 holds the same (complete) one -- same counts, and
             * the same threshold, because the threshold is decided by the
             * nearest NON-EMPTY bins either side. So the sweep visits b = 0 and
             * then lo_b+1 .. hi_b+1 and skips the rest as exact duplicates. */
            for (b = 0; b <= (lo_b <= hi_b ? hi_b + 1 : 0); b = (b ? b + 1 : lo_b + 1)) {
                double gain, cr, thr;
                size_t br;
                if (b > 0) {
                    const double *s = hb + (b - 1) * stride;
                    cl += s[0];
                    nl_cnt += s[cslot];
                    if (s[0] > 0.0)
                        prev_nz = b - 1;
                    if (cls && !newton) {
                        for (j = 0; j < ncls; j++)
                            c->cnt_left[j] += s[1 + j];
                    } else {
                        s1 += s[1];
                        s2 += s[2];
                    }
                }
                cr = tot_c - cl;
                /* minSamplesLeaf counts ROWS; cl and cr are weight sums, and
                 * the two are the same number only when nothing is weighted. */
                if (nl_cnt < (double)msl || tot_n - nl_cnt < (double)msl)
                    continue;
                if (cl <= 0.0 || cr <= 0.0)
                    continue;
                if (newton) {
                    double gr = tot_1 - s1, hr = tot_2 - s2;
                    if (s2 < mcw || hr < mcw)
                        continue;
                    gain = 0.5 * (dyn_newton_score(s1, s2, lambda, alpha) +
                                  dyn_newton_score(gr, hr, lambda, alpha) -
                                  parent) - gam;
                } else if (cls) {
                    for (j = 0; j < ncls; j++)
                        c->cnt_right[j] = c->cnt_tot[j] - c->cnt_left[j];
                    gain = parent - (dyn_tree_gini(c->cnt_left, ncls, cl) +
                                     dyn_tree_gini(c->cnt_right, ncls, cr)) -
                           gam;
                } else {
                    double r1 = tot_1 - s1, r2 = tot_2 - s2;
                    double sse_l = s2 - s1 * s1 / cl;
                    double sse_r = r2 - r1 * r1 / cr;
                    gain = parent - (sse_l + sse_r) - gam;
                }
                if (gain > *out_gain) {
                    br = (b >= lo_b && b <= hi_b + 1) ? c->hnext[b] : (uint32_t)lo_b;
                    /* prev_nz == nb means only missing rows are on the left,
                     * so no present value may descend it. */
                    if (prev_nz == nb)
                        thr = -INFINITY;
                    else if (br < nb)
                        thr = dyn_hist_mid(hi_v[prev_nz], lo_v[br]);
                    else
                        thr = hi_v[prev_nz];   /* everything present goes left */
                    *out_gain = gain;
                    *out_f = f;
                    *out_thr = thr;
                    *out_bin = (uint32_t)b;    /* bins strictly below b go left */
                    *out_mleft = dir;
                }
            }
        }
        /* Restore the invariant for the next feature: clear ONLY what this one
         * dirtied, which is the whole point of tracking the range. */
        if (lo_b <= hi_b)
            memset(hb + lo_b * stride, 0, (hi_b - lo_b + 1) * stride * sizeof(double));
        memset(hb + nb * stride, 0, stride * sizeof(double));
    }
}

/* Grow the subtree for idx[0..n) at `depth`. Returns its node index, or SIZE_MAX
 * on OOM. Children are built AFTER this node is pushed and their indices written
 * back afterwards, because pushing reallocates the node array -- holding a
 * dyn_tnode_t* across the recursion would dangle. */
static size_t dyn_tree_build(dyn_tree_ctx_t *c, dyn_tree_t *t, uint32_t *idx,
                             size_t n, size_t depth)
{
    size_t self, i, k, nf, nl, left, right, ncand;
    int32_t best_f = -1;
    double best_thr = 0.0, best_gain = 0.0;
    uint32_t best_bin = 0;
    int best_mleft = 0;
    int pure = 1;

    self = dyn_tree_push(t);
    if (self == SIZE_MAX)
        return SIZE_MAX;
    t->node[self].left = t->node[self].right = DYN_TREE_LEAF;
    t->node[self].feature = -1;
    if (c->w) {
        /* The weighted sample count, which is what the importances normalise
         * by -- so a row that counts for seven contributes seven. */
        double sw = 0.0;
        for (i = 0; i < n; i++)
            sw += c->w[idx[i]];
        t->nsamp[self] = sw;
    } else {
        t->nsamp[self] = (double)n;
    }

    for (i = 1; i < n; i++)
        if (c->y[idx[i]] != c->y[idx[0]]) {
            pure = 0;
            break;
        }
    if (pure || n < c->opt.min_samples_split ||
        n < 2 * c->opt.min_samples_leaf ||
        (c->opt.max_depth ? depth >= c->opt.max_depth
                          : depth >= DYN_TREE_MAX_DEPTH)) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }

    /* candidate features: a partial Fisher-Yates prefix of c->feat, so a
     * subsample is drawn WITHOUT replacement and reproducibly from the seed */
    ncand = c->n_cand ? c->n_cand : c->cols;
    nf = c->opt.max_features ? c->opt.max_features : ncand;
    if (nf > ncand)
        nf = ncand;
    if (nf < ncand) {
        for (i = 0; i < nf; i++) {
            size_t j = i + (size_t)(dyn_splitmix64(&c->rng) % (uint64_t)(ncand - i));
            uint32_t tmp = c->feat[i];
            c->feat[i] = c->feat[j];
            c->feat[j] = tmp;
        }
    }

    if (c->hist) {
        dyn_split_hist(c, idx, n, nf, &best_f, &best_thr, &best_gain, &best_bin,
                       &best_mleft);
        goto chosen;
    }

    for (k = 0; k < nf; k++) {
        int32_t f = (int32_t)c->feat[k];
        double sum_all = 0.0, sq_all = 0.0, parent, sum_l = 0.0, sq_l = 0.0;
        double wt_all = 0.0;
        double gain_at_entry = best_gain;

        memcpy(c->scratch, idx, n * sizeof(uint32_t));
        dyn_tree_sort_idx(c->X, c->cols, f, c->scratch, n, c->kv);

        if (c->w) {
            /* Weighted parent statistics. Separate from the unweighted arms
             * below rather than folded into them with a `w ? w[i] : 1.0`: that
             * shape puts a select in the innermost loop of every fit to serve
             * the fits that do not use it, which is exactly the cost W9.4
             * refused to pay. */
            wt_all = 0.0;
            if (!c->opt.classifier) {
                for (i = 0; i < n; i++) {
                    uint32_t r = c->scratch[i];
                    double v = c->y[r], wi = c->w[r];
                    sum_all += wi * v;
                    sq_all += wi * v * v;
                    wt_all += wi;
                }
                parent = wt_all > 0.0 ? sq_all - sum_all * sum_all / wt_all : 0.0;
            } else {
                memset(c->cnt_left, 0, c->n_classes * sizeof(double));
                memset(c->cnt_right, 0, c->n_classes * sizeof(double));
                for (i = 0; i < n; i++) {
                    uint32_t r = c->scratch[i];
                    c->cnt_right[dyn_tree_class_of(c, r)] += c->w[r];
                    wt_all += c->w[r];
                }
                parent = dyn_tree_gini(c->cnt_right, c->n_classes, wt_all);
            }
        } else if (!c->opt.classifier) {
            for (i = 0; i < n; i++) {
                double v = c->y[c->scratch[i]];
                sum_all += v;
                sq_all += v * v;
            }
            parent = sq_all - sum_all * sum_all / (double)n;
        } else {
            memset(c->cnt_left, 0, c->n_classes * sizeof(double));
            memset(c->cnt_right, 0, c->n_classes * sizeof(double));
            for (i = 0; i < n; i++)
                c->cnt_right[dyn_tree_class_of(c, c->scratch[i])] += 1.0;
            parent = dyn_tree_gini(c->cnt_right, c->n_classes, (double)n);
        }

        /* Sweep the split point, moving one sample left per step. This is the
         * hot loop of every tree, forest and boosting fit.
         *
         * Unswitched on c->opt.classifier, which is invariant across the whole
         * sweep but was tested TWICE per sample. The feature value is also
         * carried forward -- xprev at step i is exactly xcur from step i-1 --
         * which removes one gathered load and one index multiply per sample.
         * Context fields are hoisted into locals so the loop does not reload
         * them through c. Arithmetic and ordering are unchanged, so results are
         * bit-identical. */
        {
        const double *TX = c->X;
        const double *ty = c->y;
        const uint32_t *scr = c->scratch;
        size_t cols = c->cols, msl = c->opt.min_samples_leaf;
        double xprev = TX[(size_t)scr[0] * cols + f];

        if (c->w) {
            /* The weighted sweep. minSamplesLeaf still counts ROWS -- a weight
             * is how much a row matters, not how many rows it is -- so the
             * count and the weight sum are carried separately. */
            const double *tw = c->w;
            double wt_l = 0.0;
            double *cl_l = c->cnt_left, *cl_r = c->cnt_right;
            size_t ncls = c->n_classes;
            int cls = c->opt.classifier;
            for (i = 1; i < n; i++) {
                uint32_t prev = scr[i - 1];
                double xcur = TX[(size_t)scr[i] * cols + f];
                double wi = tw[prev];
                if (cls) {
                    cl_l[dyn_tree_class_of(c, prev)] += wi;
                    cl_r[dyn_tree_class_of(c, prev)] -= wi;
                } else {
                    double v = ty[prev];
                    sum_l += wi * v;
                    sq_l += wi * v * v;
                }
                wt_l += wi;
                if (xcur != xprev && i >= msl && n - i >= msl) {
                    double wt_r = wt_all - wt_l, gain;
                    if (wt_l <= 0.0 || wt_r <= 0.0) {
                        xprev = xcur;
                        continue;
                    }
                    if (cls) {
                        gain = parent - (dyn_tree_gini(cl_l, ncls, wt_l) +
                                         dyn_tree_gini(cl_r, ncls, wt_r));
                    } else {
                        double sum_r = sum_all - sum_l, sq_r = sq_all - sq_l;
                        gain = parent - ((sq_l - sum_l * sum_l / wt_l) +
                                         (sq_r - sum_r * sum_r / wt_r));
                    }
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_f = f;
                        best_thr = xprev + (xcur - xprev) * 0.5;
                        if (best_thr >= xcur)
                            best_thr = xprev;
                    }
                }
                xprev = xcur;
            }
        } else if (!c->opt.classifier) {
            for (i = 1; i < n; i++) {
                uint32_t prev = scr[i - 1];
                double xcur = TX[(size_t)scr[i] * cols + f];
                double v = ty[prev];
                sum_l += v;
                sq_l += v * v;
                if (xcur != xprev && i >= msl && n - i >= msl) {
                    double sum_r = sum_all - sum_l, sq_r = sq_all - sq_l;
                    double sse_l = sq_l - sum_l * sum_l / (double)i;
                    double sse_r = sq_r - sum_r * sum_r / (double)(n - i);
                    double gain = parent - (sse_l + sse_r);
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_f = f;
                        /* midpoint, so a test value equal to a training value
                         * is never exactly on the boundary */
                        best_thr = xprev + (xcur - xprev) * 0.5;
                        if (best_thr >= xcur) /* adjacent doubles */
                            best_thr = xprev;
                    }
                }
                xprev = xcur;
            }
        } else {
            double *cl_l = c->cnt_left, *cl_r = c->cnt_right;
            size_t ncls = c->n_classes;
            for (i = 1; i < n; i++) {
                uint32_t prev = scr[i - 1];
                double xcur = TX[(size_t)scr[i] * cols + f];
                size_t cl = dyn_tree_class_of(c, prev);
                cl_l[cl] += 1.0;
                cl_r[cl] -= 1.0;
                if (xcur != xprev && i >= msl && n - i >= msl) {
                    double gain = parent -
                           (dyn_tree_gini(cl_l, ncls, (double)i) +
                            dyn_tree_gini(cl_r, ncls, (double)(n - i)));
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_f = f;
                        best_thr = xprev + (xcur - xprev) * 0.5;
                        if (best_thr >= xcur)
                            best_thr = xprev;
                    }
                }
                xprev = xcur;
            }
        }
        }
        /* This feature won: keep its sorted order instead of re-deriving it
           with another full sort after the loop. */
        if (best_gain > gain_at_entry)
            memcpy(c->best_order, c->scratch, n * sizeof(uint32_t));
    }

chosen:
    if (best_f < 0) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }
    if (c->hist) {
        /* Stable partition by bin code -- no order to inherit, because the
         * histogram finder never sorted anything. Missing rows follow the
         * direction the sweep chose for them. */
        const dyn_hist_t *H = c->hist;
        const uint8_t *cf = H->code + (size_t)best_f * H->rows;
        uint8_t miss = (uint8_t)H->nbin[best_f];
        size_t j = 0;
        nl = 0;
        for (i = 0; i < n; i++) {
            uint8_t code = cf[idx[i]];
            int goes_left = (code == miss) ? best_mleft : (code < best_bin);
            if (goes_left)
                c->scratch[nl++] = idx[i];
        }
        j = nl;
        for (i = 0; i < n; i++) {
            uint8_t code = cf[idx[i]];
            int goes_left = (code == miss) ? best_mleft : (code < best_bin);
            if (!goes_left)
                c->scratch[j++] = idx[i];
        }
        memcpy(idx, c->scratch, n * sizeof(uint32_t));
        t->node[self].flags = best_mleft ? DYN_TNODE_MISSING_LEFT : 0;
    } else {
        /* partition in place: sorting idx by the chosen feature makes the left
         * set a prefix, and nl is recomputed from the threshold so it does not
         * depend on how ties were ordered.
         * c->best_order already holds idx sorted by best_f -- produced by the
         * sweep, by the identical sort, over the identical set. */
        memcpy(idx, c->best_order, n * sizeof(uint32_t));
        nl = 0;
        while (nl < n && c->X[(size_t)idx[nl] * c->cols + best_f] <= best_thr)
            nl++;
    }
    if (nl == 0 || nl == n) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }
    /* best_gain is already sample-weighted (both dyn_tree_gini and the SSE
     * form are pre-multiplied by their counts), so it IS the unnormalised
     * impurity decrease sklearn's feature_importances_ sums. Recorded before
     * the recursion, which reallocates the arrays. */
    t->improve[self] = best_gain;
    left = dyn_tree_build(c, t, idx, nl, depth + 1);
    if (left == SIZE_MAX)
        return SIZE_MAX;
    right = dyn_tree_build(c, t, idx + nl, n - nl, depth + 1);
    if (right == SIZE_MAX)
        return SIZE_MAX;
    t->node[self].feature = best_f;
    t->node[self].threshold = best_thr;
    t->node[self].left = (uint32_t)left;
    t->node[self].right = (uint32_t)right;
    return self;
}

/* Walk one row to its leaf. */
static double dyn_tree_predict_row(const dyn_tree_t *t, const double *x)
{
    uint32_t at = 0;
    while (t->node[at].feature >= 0) {
        at = (x[t->node[at].feature] <= t->node[at].threshold)
                 ? t->node[at].left : t->node[at].right;
    }
    return t->node[at].value;
}

/* The same walk for a model that ACCEPTS missing values: a NaN takes the
 * direction the split learned for it rather than failing the comparison and
 * silently going right.
 *
 * It is a second function rather than a flag inside the first because the walk
 * above is the hot path of every tree model in the module, and every one of
 * them rejects NaN at fit time (W9.4) -- so a per-node isnan() test there would
 * be paid by everybody to answer a question only one model asks. */
static double dyn_tree_predict_row_missing(const dyn_tree_t *t, const double *x)
{
    uint32_t at = 0;
    while (t->node[at].feature >= 0) {
        double v = x[t->node[at].feature];
        int left = isnan(v) ? (t->node[at].flags & DYN_TNODE_MISSING_LEFT)
                            : (v <= t->node[at].threshold);
        at = left ? t->node[at].left : t->node[at].right;
    }
    return t->node[at].value;
}

/* Fit one tree over the sample indices in idx[0..n) (which this consumes and
 * permutes). Returns 0, or -1 (throwing). */
/* Buffers every tree of a fit shares. One struct rather than fifteen arguments,
 * because the histogram finder and the second-order objective each need their
 * own and the parameter list was already at the limit of readability. */
typedef struct {
    uint32_t *scratch;        /* rows */
    uint32_t *feat;           /* cols */
    double *cnt_l, *cnt_r, *cnt_tot;   /* n_classes each */
    dyn_tree_kv_t *kv;        /* rows */
    uint32_t *best_order;     /* rows */
    const dyn_hist_t *hist;   /* NULL = exact split finding */
    double *hbuf;             /* (max_bins + 1) * hstride */
    uint32_t *hnext;          /* max_bins */
    size_t hstride;
    const double *grad, *hess;
    const double *w;          /* per-row sample weight, or NULL */
    const uint8_t *cls_of;    /* rows: y's class index, resolved once per fit */
    size_t n_cand;            /* usable prefix of feat; 0 = every column */
} dyn_tree_work_t;

static int dyn_tree_fit_one(JSContext *ctx, dyn_tree_t *t, const double *X,
                           const double *y, size_t rows, size_t cols,
                           const double *classes, size_t n_classes,
                           const dyn_tree_opts_t *opt, uint64_t *rng,
                           uint32_t *idx, size_t n, const dyn_tree_work_t *w)
{
    dyn_tree_ctx_t c;
    size_t i;

    (void)rows;
    c.X = X;
    c.y = y;
    c.cols = cols;
    c.classes = classes;
    c.n_classes = n_classes;
    c.cnt_left = w->cnt_l;
    c.cnt_right = w->cnt_r;
    c.cnt_tot = w->cnt_tot;
    c.feat = w->feat;
    c.n_cand = w->n_cand;
    c.scratch = w->scratch;
    c.kv = w->kv;
    c.best_order = w->best_order;
    c.hist = w->hist;
    c.hbuf = w->hbuf;
    c.hnext = w->hnext;
    c.hstride = w->hstride;
    c.grad = w->grad;
    c.hess = w->hess;
    c.w = w->w;
    c.cls_of = w->cls_of;
    c.opt = *opt;
    c.rng = *rng;
    /* A column-subsampled tree arrives with its own candidate list already in
     * feat[0..n_cand); regenerating it here would undo the draw. */
    if (!w->n_cand)
        for (i = 0; i < cols; i++)
            c.feat[i] = (uint32_t)i;
    /* Set BEFORE the first push: it is the stride of the proba array the push
     * allocates. A regressor's tree (including every GBDT tree, which fits
     * residuals) leaves it 0 and carries no proba array at all. */
    t->n_classes = opt->classifier ? n_classes : 0;
    if (dyn_tree_build(&c, t, idx, n, 0) == SIZE_MAX) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    *rng = c.rng;      /* the tree consumed RNG draws; keep the stream moving */
    return 0;
}

/* ---------- the JS classes over that one implementation --------------------- */

/* All five tree models share this shape: n_trees trees, plus the extras GBDT
 * needs (a base prediction and a learning rate; 1 tree and lr 1.0 elsewhere). */
typedef struct {
    int fitted;
    int classifier;
    int boosting;
    size_t n_trees;       /* LENGTH of the tree array: rounds x n_out */
    size_t n_rounds;      /* CONFIGURED nEstimators; what a refit uses */
    size_t n_features;
    size_t n_classes;
    double *classes;
    /* GBDT: the initial constant raw score, one per OUTPUT. A regressor and a
     * binary classifier have one output; a K-class classifier has K, and its
     * trees are laid out round-major -- trees[r * n_out + k]. */
    double *raw_base;
    size_t n_out;
    double lr;            /* GBDT: shrinkage */
    double subsample;     /* GBDT: row fraction per tree */
    /* Second-order boosting only. `colsample` draws the candidate columns once
     * per TREE, where maxFeatures draws them again at every node -- the two
     * compose, and XGBoost has both for the same reason. */
    double colsample;
    size_t early_stop;    /* rounds without validation improvement, 0 = off */
    double val_frac;      /* held-out fraction that early stopping watches */
    size_t best_rounds;   /* rounds kept; equals nEstimators when it ran out */
    dyn_tree_t *trees;
    dyn_tree_opts_t opt;
    uint64_t seed;
} dyn_forest_t;

static JSClassID dyn_dtc_class_id;
static JSClassID dyn_dtr_class_id;
static JSClassID dyn_rfc_class_id;
static JSClassID dyn_rfr_class_id;
static JSClassID dyn_gbr_class_id;
static JSClassID dyn_gbc_class_id;
static JSClassID dyn_xgbr_class_id;
static JSClassID dyn_xgbc_class_id;

static void dyn_forest_dispose(void *native)
{
    dyn_forest_t *m = (dyn_forest_t *)native;
    size_t i;
    if (!m)
        return;
    if (m->trees) {
        for (i = 0; i < m->n_trees; i++)
            dyn_tree_free(&m->trees[i]);
        free(m->trees);
    }
    free(m->classes);
    free(m->raw_base);
    free(m);
}

static const JSClassDef dyn_dtc_class = { "DecisionTreeClassifier", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_dtr_class = { "DecisionTreeRegressor", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_rfc_class = { "RandomForestClassifier", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_rfr_class = { "RandomForestRegressor", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_gbr_class = { "GradientBoostingRegressor", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_gbc_class = { "GradientBoostingClassifier", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_xgbr_class = { "XGBRegressor", .finalizer = dyn_res_finalizer };
static const JSClassDef dyn_xgbc_class = { "XGBClassifier", .finalizer = dyn_res_finalizer };

/* Read an optional positive-integer option. Absent/undefined leaves *out. */
static int dyn_opt_size(JSContext *ctx, JSValueConst obj, const char *name,
                        size_t *out, size_t minimum)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int64_t iv;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &iv, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (iv < (int64_t)minimum) {
        JS_ThrowRangeError(ctx, "%s must be at least %u", name,
                           (unsigned)minimum);
        return -1;
    }
    *out = (size_t)iv;
    return 0;
}

static int dyn_opt_double(JSContext *ctx, JSValueConst obj, const char *name,
                          double *out, double lo, double hi)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double dv;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToFloat64(ctx, &dv, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(dv >= lo && dv <= hi)) {
        JS_ThrowRangeError(ctx, "%s must be in [%g, %g]", name, lo, hi);
        return -1;
    }
    *out = dv;
    return 0;
}

/* new <TreeModel>(options?). Unknown keys are ignored (forward compatibility);
 * out-of-range values throw rather than being clamped, since silently training a
 * different model than asked for is worse than an error. */
static JSValue dyn_forest_new(JSContext *ctx, int argc, JSValueConst *argv,
                              JSClassID class_id, int classifier, int boosting,
                              size_t default_trees, size_t default_depth,
                              int newton)
{
    dyn_forest_t *m;
    size_t n_trees = default_trees, seed_holder;
    dyn_tree_opts_t opt;
    double lr = newton ? 0.3 : 0.1, subsample = 1.0, colsample = 1.0;
    size_t early_stop = 0;
    double val_frac = 0.1;
    uint64_t seed = 12345;

    memset(&opt, 0, sizeof(opt));
    opt.max_depth = default_depth;
    opt.min_samples_split = 2;
    opt.min_samples_leaf = 1;
    opt.max_features = 0;
    opt.classifier = classifier;
    opt.newton = newton;
    if (newton) {
        /* XGBoost's defaults, because a caller who knows the algorithm knows
         * these numbers. The split finder is the histogram one and is not
         * optional here: the second-order gain is defined over bin statistics,
         * and an exact sweep would be a second implementation of it. */
        opt.max_bins = DYN_HIST_MAX_BINS;
        opt.lambda = 1.0;
        opt.alpha = 0.0;
        opt.gamma = 0.0;
        opt.min_child_weight = 1.0;
    }

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        seed_holder = (size_t)seed;
        if (newton &&
            (dyn_opt_double(ctx, o, "lambda", &opt.lambda, 0.0, 1e12) ||
             dyn_opt_double(ctx, o, "alpha", &opt.alpha, 0.0, 1e12) ||
             dyn_opt_double(ctx, o, "gamma", &opt.gamma, 0.0, 1e12) ||
             dyn_opt_double(ctx, o, "minChildWeight", &opt.min_child_weight,
                            0.0, 1e12) ||
             dyn_opt_double(ctx, o, "colsampleByTree", &colsample, 1e-6, 1.0) ||
             dyn_opt_double(ctx, o, "validationFraction", &val_frac, 0.0, 0.5) ||
             dyn_opt_size(ctx, o, "earlyStoppingRounds", &early_stop, 0)))
            return JS_EXCEPTION;
        /* The first-order boosters have no validation split, so their options
         * would be accepted and never consulted -- the refused-vs-ignored rule
         * (dyn_ml_reject_weights) applies to every option, not only weights. */
        if (!newton) {
            JSValue v = JS_GetPropertyStr(ctx, o, "earlyStoppingRounds");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "earlyStoppingRounds is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, o, "validationFraction");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "validationFraction is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
        }
        if (dyn_opt_size(ctx, o, "nEstimators", &n_trees, 1) ||
            dyn_opt_size(ctx, o, "maxDepth", &opt.max_depth, 0) ||
            dyn_opt_size(ctx, o, "minSamplesSplit", &opt.min_samples_split, 2) ||
            dyn_opt_size(ctx, o, "minSamplesLeaf", &opt.min_samples_leaf, 1) ||
            dyn_opt_size(ctx, o, "maxFeatures", &opt.max_features, 0) ||
            dyn_opt_size(ctx, o, "maxBins", &opt.max_bins, 0) ||
            dyn_opt_size(ctx, o, "seed", &seed_holder, 0) ||
            dyn_opt_double(ctx, o, "learningRate", &lr, 1e-12, 1e12) ||
            dyn_opt_double(ctx, o, "subsample", &subsample, 1e-6, 1.0))
            return JS_EXCEPTION;
        seed = (uint64_t)seed_holder;
        /* 0 keeps the exact splitter. Anything else is a bin count, and it is
         * clamped nowhere: silently training on 255 bins when 4096 were asked
         * for is training a different model than the caller described. */
        if (opt.max_bins && (opt.max_bins < 2 ||
                             opt.max_bins > DYN_HIST_MAX_BINS))
            return JS_ThrowRangeError(ctx, "maxBins must be 0 (exact splits) "
                                      "or between 2 and %u",
                                      (unsigned)DYN_HIST_MAX_BINS);
        /* the exact splitter never learns the missing direction the Newton
         * objective depends on, so 0 is not an option here */
        if (newton && opt.max_bins == 0)
            return JS_ThrowRangeError(ctx, "maxBins 0 (exact splits) is not "
                                      "supported with the Newton objective");
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    if (!boosting)
        n_trees = (default_trees == 1) ? 1 : n_trees;   /* a single tree stays one */
    /* rounds x n_out trees get allocated and fitted; cap the caller-controlled
     * count before either can stall the engine */
    if (n_trees > DYN_ML_MAX_TREES)
        return JS_ThrowRangeError(ctx, "nEstimators must be at most %u",
                                  (unsigned)DYN_ML_MAX_TREES);

    m = (dyn_forest_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->classifier = classifier;
    m->boosting = boosting;
    m->n_trees = m->n_rounds = (default_trees == 1 && !boosting) ? 1 : n_trees;
    m->opt = opt;
    m->lr = lr;
    m->subsample = subsample;
    m->colsample = colsample;
    m->early_stop = early_stop;
    m->val_frac = val_frac;
    m->seed = seed;
    return dyn_res_wrap(ctx, class_id, m, dyn_forest_dispose);
}

/* Index of the leaf `x` lands in, for one tree. */
static uint32_t dyn_tree_leaf_index(const dyn_tree_t *t, const double *x,
                                    size_t cols)
{
    uint32_t i = 0;
    (void)cols;
    if (t->n_nodes == 0)
        return 0;
    while (t->node[i].feature >= 0)
        i = x[t->node[i].feature] <= t->node[i].threshold ? t->node[i].left
                                                         : t->node[i].right;
    return i;
}

static uint32_t dyn_tree_leaf_index_missing(const dyn_tree_t *t, const double *x)
{
    uint32_t i = 0;
    if (t->n_nodes == 0)
        return 0;
    while (t->node[i].feature >= 0) {
        double v = x[t->node[i].feature];
        int left = isnan(v) ? (t->node[i].flags & DYN_TNODE_MISSING_LEFT)
                            : (v <= t->node[i].threshold);
        i = left ? t->node[i].left : t->node[i].right;
    }
    return i;
}

/* Replace a boosted tree's leaf values with Friedman's one-step Newton line
 * search: the tree chose the PARTITION by least squares on the gradient, and
 * this chooses the value inside each region for the loss actually being
 * minimised. Without it a boosted classifier is fitting squared error to a
 * residual and converges to the wrong thing.
 *
 *   gamma_j = scale * sum_{i in j} grad_i / sum_{i in j} hess_i
 *
 * A leaf whose hessian sum underflows gets 0, not a division: those are leaves
 * where every sample is already predicted with near-certainty, and the ratio
 * there is numerically meaningless rather than large.
 *
 * `rows_in` are the sampled rows only -- a subsampled round must line-search on
 * the same rows it split on. Leaves that no sampled row reached keep 0. */
static void dyn_boost_leaf_newton(dyn_tree_t *t, const double *X, size_t cols,
                                  const double *grad, const double *hess,
                                  const double *w, const uint32_t *rows_in,
                                  size_t n, double scale, double *num,
                                  double *den)
{
    size_t i;
    if (t->n_nodes == 0)
        return;
    memset(num, 0, t->n_nodes * sizeof(double));
    memset(den, 0, t->n_nodes * sizeof(double));
    for (i = 0; i < n; i++) {
        uint32_t row = rows_in[i];
        uint32_t leaf = dyn_tree_leaf_index(t, X + (size_t)row * cols, cols);
        if (w) {
            num[leaf] += w[row] * grad[row];
            den[leaf] += w[row] * hess[row];
        } else {
            num[leaf] += grad[row];
            den[leaf] += hess[row];
        }
    }
    for (i = 0; i < t->n_nodes; i++) {
        if (t->node[i].feature >= 0)
            continue;
        t->node[i].value = (den[i] > 1e-12) ? scale * num[i] / den[i] : 0.0;
    }
}

/* ---- the second-order objective (W9.8b) -------------------------------------
 *
 * First-order boosting fits a least-squares tree to the negative gradient and
 * then repairs each leaf with a one-step Newton line search (dyn_boost_leaf_
 * newton). The PARTITION it chooses is therefore still a variance reduction on
 * the gradient -- the curvature only arrives afterwards, once the shape of the
 * tree is already fixed.
 *
 * The second-order objective puts the curvature in the split criterion itself.
 * Writing G and H for the gradient and hessian sums in a region, the loss
 * minimised by a constant w over that region expands to G*w + (H + lambda)w^2/2,
 * whose minimum is w = -G/(H + lambda) and whose value is -G^2/(2(H + lambda)).
 * So a split is worth
 *
 *     1/2 [ G_L^2/(H_L+l) + G_R^2/(H_R+l) - G^2/(H+l) ] - gamma
 *
 * and the leaf value is the Newton step by construction rather than by repair.
 * L1 (alpha) enters by soft-thresholding G, which is what makes a leaf weight
 * reach EXACTLY zero; minChildWeight is a floor on H rather than on a sample
 * count, so a region of near-certain predictions -- which carries little
 * curvature however many rows are in it -- cannot be split on noise.
 *
 * The loss functions and their derivatives, all with respect to the raw score F:
 *
 *   squared error   g = F - y            h = 1
 *   binary logistic g = sigma(F) - y     h = sigma(F)(1 - sigma(F))
 *   softmax, class k g = p_k - [y = k]   h = 2 p_k (1 - p_k)
 *
 * The softmax hessian is XGBoost's: the true multiclass hessian is a matrix and
 * the diagonal approximation p(1-p) understates the step, so the factor of two
 * is the standard correction. It is a heuristic and is stated as one.
 */

/* The constant every boosted model starts from, and the running raw scores it
 * implies. Shared by both objectives, because "what is the best constant
 * prediction" does not depend on how the trees after it are grown. */
static void dyn_boost_init_base(int classifier, size_t n_out, const double *y,
                                size_t rows, const double *classes,
                                const double *w, double *raw_base, double *pred)
{
    size_t i, k;
    /* The base score is the best CONSTANT prediction, so a sample weight enters
     * it exactly as it enters any other mean. Leaving it unweighted was a real
     * defect: the trees corrected a residual measured from the wrong centre, so
     * "weight 3" and "the row three times" disagreed. */
    double wtot = 0.0;
    for (i = 0; i < rows; i++)
        wtot += w ? w[i] : 1.0;
    if (wtot <= 0.0)
        wtot = 1.0;

    if (!classifier) {
        double s = 0.0;
        for (i = 0; i < rows; i++)
            s += w ? w[i] * y[i] : y[i];
        raw_base[0] = s / wtot;           /* the constant that minimises SSE */
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else if (n_out == 1) {
        /* binomial deviance: the log-odds of the positive class */
        double pos = 0.0, p;
        for (i = 0; i < rows; i++)
            if (y[i] == classes[1])
                pos += w ? w[i] : 1.0;
        p = pos / wtot;
        if (p < 1e-9) p = 1e-9;
        if (p > 1.0 - 1e-9) p = 1.0 - 1e-9;
        raw_base[0] = log(p / (1.0 - p));
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else {
        /* multinomial deviance: the log prior of each class */
        for (k = 0; k < n_out; k++) {
            double cnt = 0.0, p;
            for (i = 0; i < rows; i++)
                if (y[i] == classes[k])
                    cnt += w ? w[i] : 1.0;
            p = cnt / wtot;
            if (p < 1e-9) p = 1e-9;
            raw_base[k] = log(p);
        }
        for (i = 0; i < rows; i++)
            for (k = 0; k < n_out; k++)
                pred[i * n_out + k] = raw_base[k];
    }
}

/* Gradient and hessian of the loss at the current raw scores, for output k.
 * `w` (optional) scales both, which is exactly what a sample weight means for
 * a Newton step: it multiplies the loss. */
static void dyn_xgb_grad_hess(int classifier, size_t n_out, size_t k,
                              const double *pred, const double *y,
                              const double *classes, const double *w,
                              size_t rows, double *g, double *h)
{
    size_t i, j;

    if (!classifier) {
        for (i = 0; i < rows; i++) {
            g[i] = pred[i] - y[i];
            h[i] = 1.0;
        }
    } else if (n_out == 1) {
        for (i = 0; i < rows; i++) {
            double p = 1.0 / (1.0 + exp(-pred[i]));
            double t = (y[i] == classes[1]) ? 1.0 : 0.0;
            g[i] = p - t;
            h[i] = p * (1.0 - p);
            if (h[i] < 1e-16)
                h[i] = 1e-16;
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double *f = pred + i * n_out;
            double mx = f[0], s = 0.0, ek = 0.0, p;
            for (j = 1; j < n_out; j++)
                if (f[j] > mx) mx = f[j];
            /* Keep the k-th term from the sum: it was recomputed right after. */
            for (j = 0; j < n_out; j++) {
                double e = exp(f[j] - mx);
                if (j == k) ek = e;
                s += e;
            }
            p = ek / s;
            g[i] = p - ((y[i] == classes[k]) ? 1.0 : 0.0);
            h[i] = 2.0 * p * (1.0 - p);
            if (h[i] < 1e-16)
                h[i] = 1e-16;
        }
    }
    if (w) {
        for (i = 0; i < rows; i++) {
            g[i] *= w[i];
            h[i] *= w[i];
        }
    }
}

/* Mean loss over the held-out rows, for early stopping. Same three losses. */
static double dyn_xgb_val_loss(int classifier, size_t n_out, const double *pred,
                               const double *y, const double *classes,
                               const uint8_t *isval, size_t rows)
{
    size_t i, k, nv = 0;
    double acc = 0.0;

    for (i = 0; i < rows; i++) {
        if (!isval[i])
            continue;
        nv++;
        if (!classifier) {
            double d = pred[i] - y[i];
            acc += d * d;
        } else if (n_out == 1) {
            double p = 1.0 / (1.0 + exp(-pred[i]));
            double t = (y[i] == classes[1]) ? 1.0 : 0.0;
            if (p < 1e-15) p = 1e-15;
            if (p > 1.0 - 1e-15) p = 1.0 - 1e-15;
            acc += t ? -log(p) : -log(1.0 - p);
        } else {
            const double *f = pred + i * n_out;
            double mx = f[0], s = 0.0, own = 0.0;
            for (k = 1; k < n_out; k++)
                if (f[k] > mx) mx = f[k];
            for (k = 0; k < n_out; k++)
                s += exp(f[k] - mx);
            for (k = 0; k < n_out; k++)
                if (y[i] == classes[k])
                    own = f[k] - mx;
            acc += -(own - log(s));
        }
    }
    return nv ? acc / (double)nv : 0.0;
}

/* Fit every tree. For a forest each tree sees a bootstrap resample; for boosting
 * each tree sees the gradient of the loss at the running prediction. */
static int dyn_forest_learn(JSContext *ctx, dyn_forest_t *m, const double *X,
                            const double *y, size_t rows, size_t cols,
                            const double *w)
{
    dyn_tree_t *trees = NULL;
    uint32_t *idx = NULL, *scratch = NULL, *feat = NULL, *train = NULL;
    uint8_t *isval = NULL;
    dyn_tree_kv_t *kv = NULL;
    uint32_t *best_order = NULL;
    double *cnt_l = NULL, *cnt_r = NULL, *cnt_tot = NULL, *classes = NULL;
    double *resid = NULL, *pred = NULL, *hess = NULL, *raw_base = NULL;
    double *lnum = NULL, *lden = NULL, *hbuf = NULL, *binvals = NULL;
    size_t lnum_cap = 0;
    uint32_t *hnext = NULL;
    uint8_t *cls_of = NULL;
    size_t n_classes = 1, i, ti, use_n, n_out = 1, n_alloc, rounds, n_keep;
    uint64_t rng = m->seed;
    dyn_tree_opts_t opt = m->opt;
    dyn_hist_t hist = {0};
    dyn_tree_work_t work;
    int have_hist = 0, rc = -1;

    if (m->classifier) {
        n_classes = dyn_ml_classes(ctx, y, rows, &classes);
        if (n_classes == 0)
            return -1;
        /* cls_of is one byte per row, so a class index past 255 would wrap and
         * silently relabel rows; refuse rather than train a corrupted model */
        if (n_classes > 256) {
            free(classes);
            JS_ThrowRangeError(ctx,
                "tree models support at most 256 classes, found %u",
                (unsigned)n_classes);
            return -1;
        }
    }
    /* the CONFIGURED count, never m->n_trees: a boosted classifier's tree
     * array is rounds x outputs, so reading the round count back out of it
     * would multiply by the output count again on every refit */
    rounds = m->n_rounds;
    if (m->boosting && m->classifier) {
        /* Boosting fits a LOG-ODDS, which needs two outcomes to be a ratio of.
         * A tree classifier can happily memorise a single-label training set;
         * this cannot, and a degenerate branch that pretends otherwise reads
         * past a one-element class table. */
        if (n_classes < 2) {
            free(classes);
            JS_ThrowTypeError(ctx, "%s needs at least two classes in y, "
                                   "found %u",
                              opt.newton ? "XGBClassifier"
                                         : "GradientBoostingClassifier",
                              (unsigned)n_classes);
            return -1;
        }
        /* One tree per round for two classes (the binomial deviance has a
         * single degree of freedom), one per class beyond that. */
        n_out = (n_classes == 2) ? 1 : n_classes;
    }
    n_alloc = m->boosting ? rounds * n_out : rounds;
    n_keep = n_alloc;
    trees = (dyn_tree_t *)calloc(n_alloc ? n_alloc : 1, sizeof(*trees));
    idx = (uint32_t *)malloc(rows * sizeof(uint32_t));
    scratch = (uint32_t *)malloc(rows * sizeof(uint32_t));
    kv = (dyn_tree_kv_t *)malloc(rows * sizeof(dyn_tree_kv_t));
    best_order = (uint32_t *)malloc(rows * sizeof(uint32_t));
    feat = (uint32_t *)malloc(cols * sizeof(uint32_t));
    cnt_l = (double *)malloc(n_classes * sizeof(double));
    cnt_r = (double *)malloc(n_classes * sizeof(double));
    cnt_tot = (double *)malloc(n_classes * sizeof(double));
    /* One byte per row saying which class it is, resolved here instead of by a
     * linear scan of the class table inside every split loop. Only a classifier
     * has one; a regressor's trees never ask. */
    if (opt.classifier && classes) {
        cls_of = (uint8_t *)malloc(rows ? rows : 1);
        if (cls_of) {
            for (i = 0; i < rows; i++) {
                size_t kk;
                cls_of[i] = 0;
                for (kk = 0; kk < n_classes; kk++)
                    if (classes[kk] == y[i]) { cls_of[i] = (uint8_t)kk; break; }
            }
        }
    }
    if (m->boosting) {
        /* rows is ingest-bounded, so this can only wrap when n_out > 1;
         * still, a wrapped malloc that returns small is a heap overflow */
        if (rows > (SIZE_MAX / sizeof(double)) / n_out) {
            JS_ThrowRangeError(ctx, "data too large for boosting");
            goto done;
        }
        resid = (double *)malloc(rows * sizeof(double));
        pred = (double *)malloc(rows * n_out * sizeof(double));
        raw_base = (double *)calloc(n_out, sizeof(double));
        if (m->classifier || opt.newton)
            hess = (double *)malloc(rows * sizeof(double));
    }
    if (opt.newton) {
        isval = (uint8_t *)calloc(rows, sizeof(uint8_t));
        train = (uint32_t *)malloc(rows * sizeof(uint32_t));
    }
    if (!trees || !idx || !scratch || !kv || !best_order || !feat || !cnt_l ||
        !cnt_r || !cnt_tot ||
        (opt.classifier && classes && !cls_of) ||
        (opt.newton && (!isval || !train)) ||
        (m->boosting && (!resid || !pred || !raw_base)) ||
        (m->boosting && (m->classifier || opt.newton) && !hess)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memset(&work, 0, sizeof(work));
    /* The second-order objective folds the weights into g and h before the tree
     * ever sees them, so handing them to the splitter as well would apply each
     * one twice. */
    work.w = opt.newton ? NULL : w;
    work.cls_of = cls_of;
    work.scratch = scratch;
    work.feat = feat;
    work.cnt_l = cnt_l;
    work.cnt_r = cnt_r;
    work.cnt_tot = cnt_tot;
    work.kv = kv;
    work.best_order = best_order;
    /* Bin ONCE for the whole fit: every tree of a forest resamples the same X,
     * so the codes and edges are shared and the sort is paid a single time
     * rather than once per (node, feature). */
    if (opt.max_bins) {
        /* [count, payload]: per-class counts for Gini, (sum, sum of squares)
         * for variance, (sum g, sum h) for the second-order objective. A
         * boosted classifier's TREES are regressors on the gradient, so the
         * buffer has to cover both shapes -- hence the max, not the arm that
         * matches this model's own criterion. */
        size_t payload = (opt.classifier && n_classes > 2) ? n_classes : 2;
        /* One more slot when weighted, for the row count -- see dyn_split_hist */
        size_t stride = 1 + payload + ((w && !opt.newton) ? 1 : 0);
        binvals = (double *)malloc(rows * sizeof(double));
        if (!binvals) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        if (dyn_hist_build(&hist, X, rows, cols, opt.max_bins, kv, binvals,
                           opt.newton ? NULL : w)) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        have_hist = 1;
        /* calloc, not malloc: dyn_split_hist keeps the buffer clean as an
         * invariant and only clears the range it dirtied, so the FIRST use
         * needs it already zero. */
        hbuf = (double *)calloc((hist.max_bins + 1) * stride, sizeof(double));
        /* +2, not +1: dyn_split_hist writes hnext[hi_b + 1] where hi_b can be
         * the missing bin == max_bins, one past the real bins */
        hnext = (uint32_t *)malloc((hist.max_bins + 2) * sizeof(uint32_t));
        if (!hbuf || !hnext) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        work.hist = &hist;
        work.hbuf = hbuf;
        work.hnext = hnext;
        work.hstride = stride;
    }
    /* default maxFeatures: sqrt(cols) for a forest classifier, cols/3 for a
     * forest regressor (the scikit-learn defaults), all features for a single
     * tree and for boosting */
    if (opt.max_features == 0 && m->n_rounds > 1 && !m->boosting) {
        double r = m->classifier ? sqrt((double)cols) : (double)cols / 3.0;
        size_t v = (size_t)(r + 0.5);
        opt.max_features = v ? v : 1;
    }

    if (opt.newton) {
        /* ---- second-order boosting (XGBRegressor / XGBClassifier) ---- */
        dyn_tree_opts_t topt = opt;
        size_t r, k, ntrain, nval = 0, kept = rounds, since = 0, best_round = 0;
        double best_loss = 0.0;
        int have_best = 0;

        topt.classifier = 0;      /* the trees fit gradients, not labels */
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
                            raw_base, pred);

        /* Held-out rows are drawn ONCE, so every round is judged on the same
         * data. Judging each round on a fresh sample would make the comparison
         * between rounds measure the sample rather than the model. */
        if (m->early_stop && m->val_frac > 0.0 && rows >= 4) {
            nval = (size_t)((double)rows * m->val_frac + 0.5);
            if (nval < 1)
                nval = 1;
            if (nval > rows / 2)
                nval = rows / 2;
            for (i = 0; i < rows; i++)
                idx[i] = (uint32_t)i;
            for (i = 0; i < nval; i++) {
                size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(rows - i));
                uint32_t tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
            for (i = 0; i < nval; i++)
                isval[idx[i]] = 1;
        }
        ntrain = 0;
        for (i = 0; i < rows; i++)
            if (!isval[i])
                train[ntrain++] = (uint32_t)i;

        use_n = (size_t)((double)ntrain * m->subsample + 0.5);
        if (use_n < 1)
            use_n = 1;
        if (use_n > ntrain)
            use_n = ntrain;

        for (r = 0; r < rounds; r++) {
            memcpy(idx, train, ntrain * sizeof(uint32_t));
            if (use_n < ntrain) {
                for (i = 0; i < use_n; i++) {
                    size_t j = i + (size_t)(dyn_splitmix64(&rng) %
                                            (uint64_t)(ntrain - i));
                    uint32_t tmp = idx[i];
                    idx[i] = idx[j];
                    idx[j] = tmp;
                }
            }
            for (k = 0; k < n_out; k++) {
                dyn_tree_t *tr = &trees[r * n_out + k];
                size_t ncand = (size_t)((double)cols * m->colsample + 0.5);
                if (ncand < 1)
                    ncand = 1;
                if (ncand > cols)
                    ncand = cols;
                for (i = 0; i < cols; i++)
                    feat[i] = (uint32_t)i;
                if (ncand < cols) {
                    for (i = 0; i < ncand; i++) {
                        size_t j = i + (size_t)(dyn_splitmix64(&rng) %
                                                (uint64_t)(cols - i));
                        uint32_t tmp = feat[i];
                        feat[i] = feat[j];
                        feat[j] = tmp;
                    }
                }
                work.n_cand = ncand;
                /* Gradients over EVERY row, not just the sampled ones: the
                 * held-out rows need their raw scores advanced too, or the
                 * validation loss would be measured against round zero. */
                dyn_xgb_grad_hess(m->classifier, n_out, k, pred, y, classes, w,
                                  rows, resid, hess);
                work.grad = resid;
                work.hess = hess;
                if (dyn_tree_fit_one(ctx, tr, X, resid, rows, cols, NULL, 1,
                                     &topt, &rng, idx, use_n, &work))
                    goto done;
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr *
                        dyn_tree_predict_row_missing(tr, X + i * cols);
            }
            if (nval) {
                double loss = dyn_xgb_val_loss(m->classifier, n_out, pred, y,
                                               classes, isval, rows);
                if (!have_best || loss < best_loss - 1e-12) {
                    best_loss = loss;
                    best_round = r + 1;
                    have_best = 1;
                    since = 0;
                } else if (++since >= m->early_stop) {
                    break;
                }
            }
        }
        /* Early stopping keeps the BEST round, not the last one it tried:
         * the rounds after the best are the evidence that it was the best. */
        if (have_best)
            kept = best_round;
        n_keep = kept * n_out;
        m->best_rounds = kept;
    } else if (m->boosting) {
        dyn_tree_opts_t topt = opt;
        size_t r, k;
        /* Every boosted tree fits a GRADIENT, which is a real number even when
         * the model classifies. A classification impurity here would be
         * splitting on the wrong criterion entirely. */
        topt.classifier = 0;
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
                            raw_base, pred);
        use_n = (size_t)((double)rows * m->subsample + 0.5);
        if (use_n < 1)
            use_n = 1;
        if (use_n > rows)
            use_n = rows;
        for (r = 0; r < rounds; r++) {
            /* row subsample without replacement: shuffle a prefix. One draw
             * per ROUND, so every output of a round splits on the same rows. */
            for (i = 0; i < rows; i++)
                idx[i] = (uint32_t)i;
            if (use_n < rows) {
                for (i = 0; i < use_n; i++) {
                    size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(rows - i));
                    uint32_t t = idx[i];
                    idx[i] = idx[j];
                    idx[j] = t;
                }
            }
            for (k = 0; k < n_out; k++) {
                dyn_tree_t *tr = &trees[r * n_out + k];
                double scale = 1.0;
                if (!m->classifier) {
                    for (i = 0; i < rows; i++)
                        resid[i] = y[i] - pred[i];
                } else if (n_out == 1) {
                    for (i = 0; i < rows; i++) {
                        double p = 1.0 / (1.0 + exp(-pred[i]));
                        resid[i] = ((y[i] == classes[1]) ? 1.0 : 0.0) - p;
                        hess[i] = p * (1.0 - p);
                    }
                } else {
                    /* softmax over the row's K raw scores, shifted by the max
                     * so exp() cannot overflow on a confident prediction */
                    for (i = 0; i < rows; i++) {
                        const double *f = pred + i * n_out;
                        double mx = f[0], s = 0.0, p;
                        size_t j;
                        for (j = 1; j < n_out; j++)
                            if (f[j] > mx) mx = f[j];
                        for (j = 0; j < n_out; j++)
                            s += exp(f[j] - mx);
                        p = exp(f[k] - mx) / s;
                        resid[i] = ((y[i] == classes[k]) ? 1.0 : 0.0) - p;
                        hess[i] = fabs(resid[i]) * (1.0 - fabs(resid[i]));
                    }
                    scale = (double)(n_out - 1) / (double)n_out;
                }
                if (dyn_tree_fit_one(ctx, tr, X, resid, rows, cols, NULL, 1,
                                     &topt, &rng, idx, use_n, &work))
                    goto done;
                if (m->classifier) {
                    /* the partition came from least squares on the gradient;
                     * the value inside it comes from the real loss. The pair
                     * arrays grow geometrically: realloc per round to the
                     * current node count churned the allocator n_trees times. */
                    if (tr->n_nodes > lnum_cap) {
                        double *nn, *dd;
                        size_t want = lnum_cap ? lnum_cap : 16;
                        while (want < tr->n_nodes && want <= SIZE_MAX / 16)
                            want *= 2;
                        if (want < tr->n_nodes)
                            want = tr->n_nodes;
                        nn = (double *)realloc(lnum, want * sizeof(double));
                        if (!nn) { JS_ThrowOutOfMemory(ctx); goto done; }
                        lnum = nn;
                        dd = (double *)realloc(lden, want * sizeof(double));
                        if (!dd) { JS_ThrowOutOfMemory(ctx); goto done; }
                        lden = dd;
                        lnum_cap = want;
                    }
                    dyn_boost_leaf_newton(tr, X, cols, resid, hess, w, idx,
                                          use_n, scale, lnum, lden);
                }
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr *
                        dyn_tree_predict_row(tr, X + i * cols);
            }
        }
    } else {
        for (ti = 0; ti < rounds; ti++) {
            if (rounds == 1) {
                for (i = 0; i < rows; i++)
                    idx[i] = (uint32_t)i;
            } else {
                /* bootstrap: rows draws WITH replacement */
                for (i = 0; i < rows; i++)
                    idx[i] = (uint32_t)(dyn_splitmix64(&rng) % (uint64_t)rows);
            }
            if (dyn_tree_fit_one(ctx, &trees[ti], X, y, rows, cols, classes,
                                 n_classes, &opt, &rng, idx, rows, &work))
                goto done;
        }
    }
    /* commit */
    if (m->trees) {
        for (i = 0; i < m->n_trees; i++)
            dyn_tree_free(&m->trees[i]);
        free(m->trees);
    }
    free(m->classes);
    free(m->raw_base);
    /* Early stopping grew rounds it then discarded. They are freed here rather
     * than left in the array, because dispose() frees by n_trees and anything
     * past it would leak silently. */
    for (i = n_keep; i < n_alloc; i++)
        dyn_tree_free(&trees[i]);
    m->trees = trees;
    m->classes = classes;
    m->n_classes = n_classes;
    m->n_features = cols;
    m->n_trees = n_keep;
    m->n_out = m->boosting ? n_out : 0;
    m->raw_base = raw_base;
    m->fitted = 1;
    m->opt.max_features = opt.max_features;
    if (!opt.newton)
        m->best_rounds = rounds;
    trees = NULL;
    classes = NULL;
    raw_base = NULL;
    rc = 0;
done:
    if (trees) {
        /* n_alloc, NOT m->n_trees: a refit can change the tree count (a
         * classifier with a different number of labels boosts a different
         * number of trees per round), and freeing the new array by the old
         * count walks off the end of it. */
        for (i = 0; i < n_alloc; i++)
            dyn_tree_free(&trees[i]);
        free(trees);
    }
    free(classes);
    free(idx); free(scratch); free(kv); free(best_order); free(feat);
    free(cnt_l); free(cnt_r); free(cnt_tot);
    free(resid); free(pred); free(hess); free(raw_base); free(lnum); free(lden);
    free(hbuf); free(binvals); free(hnext); free(isval); free(train);
    free(cls_of);
    if (have_hist)
        dyn_hist_free(&hist);
    return rc;
}

/* `newton` is a property of the CLASS, not of the instance, so it comes from
 * the call site rather than from the model: reading it off the model would mean
 * resolving the handle before coercing the arguments, and a coercion can close
 * the model (CLAUDE.md section 8). It decides whether a NaN in X is missing
 * data or an error. Every tree model takes sampleWeight. */
static JSValue dyn_forest_fit(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, JSClassID class_id,
                              int newton)
{
    dyn_forest_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL, *w = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_ex(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y,
                            newton))
        return JS_EXCEPTION;
    /* The split finders cast the feature index through int32_t (dyn_split_hist,
     * dyn_tree_build) and the stored node feature is int32_t; a column count
     * >= 2^31 truncates negative, reads OOB, and is misread as a leaf at
     * predict time. */
    if (mx.cols > (size_t)INT32_MAX) {
        dyn_matrix_free(&mx); free(y);
        return JS_ThrowRangeError(ctx, "too many columns for a tree model");
    }
    if (dyn_ml_ingest_weights(ctx, argc, argv, 2, mx.rows, &w)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y); free(w);
        return JS_EXCEPTION;
    }
    if (dyn_forest_learn(ctx, m, mx.data, y, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx); free(y); free(w);
        return JS_EXCEPTION;
    }
    free(w);
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

/* The boosted raw score of one row for output k: the initial constant plus the
 * shrunk sum of that output's trees. Trees are round-major, so output k's are
 * strided by n_out. */
static double dyn_forest_raw_score(const dyn_forest_t *m, const double *x,
                                   size_t k)
{
    double acc = m->raw_base[k];
    size_t r, rounds = m->n_out ? m->n_trees / m->n_out : 0;
    /* Split rather than branched per node: whether this model understands a
     * missing value is fixed for the whole walk. */
    if (m->opt.newton) {
        for (r = 0; r < rounds; r++)
            acc += m->lr *
                dyn_tree_predict_row_missing(&m->trees[r * m->n_out + k], x);
        return acc;
    }
    for (r = 0; r < rounds; r++)
        acc += m->lr * dyn_tree_predict_row(&m->trees[r * m->n_out + k], x);
    return acc;
}

static JSValue dyn_forest_predict(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv,
                                  JSClassID class_id)
{
    dyn_forest_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL, *votes = NULL;
    size_t rows, i, ti, c;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    out = (double *)malloc(rows * sizeof(double));
    if (m->classifier)
        votes = (double *)calloc(m->n_classes, sizeof(double));
    if (!out || (m->classifier && !votes)) {
        free(out); free(votes);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        if (m->boosting && m->classifier) {
            /* argmax of the raw scores -- monotone in the softmax, so this IS
             * argmax(predictProba) and the two cannot disagree */
            size_t k, best = 0;
            double bestf = 0.0;
            for (k = 0; k < m->n_out; k++) {
                double f = dyn_forest_raw_score(m, xi, k);
                if (k == 0 || f > bestf) { bestf = f; best = k; }
            }
            out[i] = (m->n_out == 1) ? m->classes[bestf > 0.0 ? 1 : 0]
                                     : m->classes[best];
        } else if (m->boosting) {
            double acc = m->raw_base[0];
            for (ti = 0; ti < m->n_trees; ti++)
                acc += m->lr * dyn_tree_predict_row(&m->trees[ti], xi);
            out[i] = acc;
        } else if (!m->classifier) {
            double acc = 0.0;
            for (ti = 0; ti < m->n_trees; ti++)
                acc += dyn_tree_predict_row(&m->trees[ti], xi);
            out[i] = acc / (double)m->n_trees;
        } else {
            /* ARGMAX OF THE AVERAGED LEAF DISTRIBUTIONS -- the same quantity
             * predictProba returns, so the two APIs cannot answer the same
             * question differently. This replaces a majority vote over hard
             * labels, which disagreed with predictProba on 4-7 of 400 rows at
             * every forest size and was the more error-prone of the two on
             * that data (0.9350 against 0.9525). Two APIs that disagree is a
             * defect, and this is the direction scikit-learn settled on for
             * the same reason.
             *
             * A single tree cannot be affected: with one tree the averaged
             * distribution is that tree's own leaf distribution, whose argmax
             * IS the majority label it would have voted. The change is visible
             * only where trees disagree, which is exactly where averaging the
             * evidence beats counting the votes. */
            size_t best = 0;
            memset(votes, 0, m->n_classes * sizeof(double));
            for (ti = 0; ti < m->n_trees; ti++) {
                const dyn_tree_t *t = &m->trees[ti];
                uint32_t leaf;
                if (t->n_classes != m->n_classes || !t->proba) {
                    /* No leaf distribution recorded: fall back to this tree's
                     * hard label so a model that predates the metadata still
                     * predicts rather than abstaining. */
                    double lab = dyn_tree_predict_row(t, xi);
                    for (c = 0; c < m->n_classes; c++)
                        if (m->classes[c] == lab) { votes[c] += 1.0; break; }
                    continue;
                }
                leaf = dyn_tree_leaf_index(t, xi, mx.cols);
                for (c = 0; c < m->n_classes; c++)
                    votes[c] += t->proba[(size_t)leaf * m->n_classes + c];
            }
            for (c = 1; c < m->n_classes; c++)
                if (votes[c] > votes[best])
                    best = c;      /* strict >: ties go to the smaller label */
            out[i] = m->classes[best];
        }
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    free(votes);
    result = dyn_ml_doubles_to_js(ctx, out, rows);
    free(out);
    return result;
}

/* predictProba(X) -> rows x n_classes, columns in `classes` order.
 *
 * The average of the LEAF CLASS DISTRIBUTIONS across trees, not the fraction
 * of trees voting for each label. The difference matters most where the
 * feature is most useful: a single DecisionTreeClassifier votes 0 or 1, so a
 * vote fraction would give it exactly two probability levels and make logLoss,
 * rocAuc and averagePrecision meaningless for it. The leaf distribution is
 * already computed at fit time (dyn_tree_leaf_value counts the classes to pick
 * the majority), so this costs nothing extra to record. */
static JSValue dyn_forest_predict_proba(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv,
                                        JSClassID class_id)
{
    dyn_forest_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL;
    size_t rows, i, ti, c, k;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predictProba before fit");
    }
    if (!m->classifier) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "predictProba is for classifiers; this model predicts a number");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    k = m->n_classes;
    out = (double *)calloc(rows * (k ? k : 1), sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        if (m->boosting) {
            /* A boosted classifier has no leaf distributions to average: its
             * trees fit gradients, and the probability IS the link function of
             * the raw score. Binomial -> sigmoid; multinomial -> softmax,
             * shifted by the max so a confident row cannot overflow exp(). */
            if (m->n_out == 1) {
                double p = 1.0 / (1.0 + exp(-dyn_forest_raw_score(m, xi, 0)));
                out[i * k + 0] = 1.0 - p;
                out[i * k + 1] = p;
            } else {
                double mxf = 0.0, s = 0.0;
                for (c = 0; c < k; c++) {
                    out[i * k + c] = dyn_forest_raw_score(m, xi, c);
                    if (c == 0 || out[i * k + c] > mxf)
                        mxf = out[i * k + c];
                }
                for (c = 0; c < k; c++) {
                    out[i * k + c] = exp(out[i * k + c] - mxf);
                    s += out[i * k + c];
                }
                for (c = 0; c < k; c++)
                    out[i * k + c] /= s;
            }
            continue;
        }
        for (ti = 0; ti < m->n_trees; ti++) {
            const dyn_tree_t *t = &m->trees[ti];
            uint32_t leaf;
            if (t->n_classes != k || !t->proba)
                continue;
            leaf = dyn_tree_leaf_index(t, xi, mx.cols);
            for (c = 0; c < k; c++)
                out[i * k + c] += t->proba[(size_t)leaf * k + c];
        }
        for (c = 0; c < k; c++)
            out[i * k + c] /= (double)m->n_trees;
    }
    dyn_matrix_free(&mx);
    result = dyn_ml_matrix_to_js(ctx, out, rows, k, 0);
    free(out);
    return result;
}

/* apply(X) -> rows x n_trees leaf indices. The identity of the leaf a row
 * lands in is the tree's own encoding of that row, which is what makes it
 * useful as a feature for a downstream model. */
static JSValue dyn_forest_apply(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv,
                                JSClassID class_id)
{
    dyn_forest_t *m;
    dyn_matrix_t mx = {0};
    double *out;
    size_t rows, i, ti;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "apply before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    out = (double *)malloc(rows * (m->n_trees ? m->n_trees : 1) *
                           sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        for (ti = 0; ti < m->n_trees; ti++)
            out[i * m->n_trees + ti] = (double)(m->opt.newton
                ? dyn_tree_leaf_index_missing(&m->trees[ti], xi)
                : dyn_tree_leaf_index(&m->trees[ti], xi, mx.cols));
    }
    dyn_matrix_free(&mx);
    result = dyn_ml_matrix_to_js(ctx, out, rows, m->n_trees, 0);
    free(out);
    return result;
}

/* featureImportances -> one weight per feature, summing to 1.
 *
 * The Gini/variance importance: for each split, the impurity decrease it
 * achieved weighted by the samples that reached it, divided by the tree's root
 * sample count, summed per feature, averaged over trees, then normalised.
 *
 * It is worth knowing what this number is NOT. It is computed from the
 * training fit alone, so it rewards features with many distinct values (they
 * offer more split points), and among correlated features it credits whichever
 * one happened to be chosen first. It is a description of how the forest was
 * built, not a measurement of what matters. */
static JSValue dyn_forest_importances(JSContext *ctx, JSValueConst this_val,
                                      JSClassID class_id)
{
    dyn_forest_t *m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    double *imp, total = 0.0;
    size_t ti, i, f;
    JSValue result;

    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_ThrowInternalError(ctx, "featureImportances before fit");
    imp = (double *)calloc(m->n_features ? m->n_features : 1, sizeof(double));
    if (!imp)
        return JS_ThrowOutOfMemory(ctx);
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t *t = &m->trees[ti];
        double root = t->n_nodes ? t->nsamp[0] : 0.0;
        if (root <= 0.0)
            continue;
        for (i = 0; i < t->n_nodes; i++) {
            f = (size_t)t->node[i].feature;
            if (t->node[i].feature < 0 || f >= m->n_features)
                continue;
            imp[f] += t->improve[i] / root;
        }
    }
    for (f = 0; f < m->n_features; f++)
        total += imp[f];
    /* A forest that never split (every tree a single leaf) has no basis for a
     * preference; a uniform vector would invent one, so report zeros. */
    if (total > 0.0)
        for (f = 0; f < m->n_features; f++)
            imp[f] /= total;
    result = dyn_ml_doubles_to_js(ctx, imp, m->n_features);
    free(imp);
    return result;
}

static JSValue dyn_forest_depth(JSContext *ctx, JSValueConst this_val,
                                JSClassID class_id)
{
    dyn_forest_t *m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    size_t ti, i, maxd = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    /* depth of the deepest tree, derived by walking the flat node array: a node's
     * depth is one more than its parent's, and parents precede children */
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t *t = &m->trees[ti];
        uint32_t *d = (uint32_t *)calloc(t->n_nodes ? t->n_nodes : 1,
                                         sizeof(uint32_t));
        if (!d)
            return JS_ThrowOutOfMemory(ctx);
        for (i = 0; i < t->n_nodes; i++) {
            if (t->node[i].feature < 0) {
                if (d[i] > maxd)
                    maxd = d[i];
                continue;
            }
            d[t->node[i].left] = d[i] + 1;
            d[t->node[i].right] = d[i] + 1;
        }
        free(d);
    }
    return JS_NewInt32(ctx, (int)maxd);
}

#define DYN_FOREST_FNS(prefix, class_id_var, newton_model)                     \
static JSValue prefix##_fit(JSContext *ctx, JSValueConst t, int argc,           \
                            JSValueConst *argv)                                \
{ return dyn_forest_fit(ctx, t, argc, argv, class_id_var, newton_model); }      \
static JSValue prefix##_predict(JSContext *ctx, JSValueConst t, int argc,       \
                                JSValueConst *argv)                            \
{ return dyn_forest_predict(ctx, t, argc, argv, class_id_var); }                \
static JSValue prefix##_depth(JSContext *ctx, JSValueConst t)                   \
{ return dyn_forest_depth(ctx, t, class_id_var); }                              \
static JSValue prefix##_proba(JSContext *ctx, JSValueConst t, int argc,        \
                              JSValueConst *argv)                              \
{ return dyn_forest_predict_proba(ctx, t, argc, argv, class_id_var); }          \
static JSValue prefix##_apply(JSContext *ctx, JSValueConst t, int argc,        \
                              JSValueConst *argv)                              \
{ return dyn_forest_apply(ctx, t, argc, argv, class_id_var); }                  \
static JSValue prefix##_imp(JSContext *ctx, JSValueConst t)                     \
{ return dyn_forest_importances(ctx, t, class_id_var); }

/* The prototype is separate from the methods because the second-order models
 * share every method and add one accessor. */
#define DYN_FOREST_PROTO(prefix)                                               \
static const JSCFunctionListEntry prefix##_proto[] = {                         \
    JS_CFUNC_DEF("fit", 2, prefix##_fit),                                      \
    JS_CFUNC_DEF("predict", 1, prefix##_predict),                              \
    JS_CFUNC_DEF("predictProba", 1, prefix##_proba),                           \
    JS_CFUNC_DEF("apply", 1, prefix##_apply),                                  \
    JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL),                  \
    JS_CGETSET_DEF("depth", prefix##_depth, NULL),                             \
};

#define DYN_FOREST_METHODS(prefix, class_id_var, newton_model)                 \
    DYN_FOREST_FNS(prefix, class_id_var, newton_model)                         \
    DYN_FOREST_PROTO(prefix)

DYN_FOREST_METHODS(dyn_dtc, dyn_dtc_class_id, 0)
DYN_FOREST_METHODS(dyn_dtr, dyn_dtr_class_id, 0)
DYN_FOREST_METHODS(dyn_rfc, dyn_rfc_class_id, 0)
DYN_FOREST_METHODS(dyn_rfr, dyn_rfr_class_id, 0)
DYN_FOREST_METHODS(dyn_gbr, dyn_gbr_class_id, 0)
DYN_FOREST_METHODS(dyn_gbc, dyn_gbc_class_id, 0)
DYN_FOREST_FNS(dyn_xgbr, dyn_xgbr_class_id, 1)
DYN_FOREST_FNS(dyn_xgbc, dyn_xgbc_class_id, 1)

/* Rounds the model actually kept. Without early stopping it is nEstimators;
 * with it, the round whose validation loss was lowest -- and that number is the
 * result of the fit, not a diagnostic, because it is what the model IS. */
static JSValue dyn_xgb_best(JSContext *ctx, JSValueConst this_val,
                            JSClassID class_id)
{
    dyn_forest_t *m = (dyn_forest_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->best_rounds : 0));
}

#define DYN_XGB_PROTO(prefix, class_id_var)                                    \
static JSValue prefix##_best(JSContext *ctx, JSValueConst t)                   \
{ return dyn_xgb_best(ctx, t, class_id_var); }                                 \
static const JSCFunctionListEntry prefix##_xproto[] = {                        \
    JS_CFUNC_DEF("fit", 2, prefix##_fit),                                      \
    JS_CFUNC_DEF("predict", 1, prefix##_predict),                              \
    JS_CFUNC_DEF("predictProba", 1, prefix##_proba),                           \
    JS_CFUNC_DEF("apply", 1, prefix##_apply),                                  \
    JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL),                  \
    JS_CGETSET_DEF("depth", prefix##_depth, NULL),                             \
    JS_CGETSET_DEF("bestRounds", prefix##_best, NULL),                         \
};

DYN_XGB_PROTO(dyn_xgbr, dyn_xgbr_class_id)
DYN_XGB_PROTO(dyn_xgbc, dyn_xgbc_class_id)

static JSValue dyn_dtc_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_dtc_class_id, 1, 0, 1, 0, 0); }
static JSValue dyn_dtr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_dtr_class_id, 0, 0, 1, 0, 0); }
static JSValue dyn_rfc_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_rfc_class_id, 1, 0, 100, 0, 0); }
static JSValue dyn_rfr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_rfr_class_id, 0, 0, 100, 0, 0); }
static JSValue dyn_gbr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_gbr_class_id, 0, 1, 100, 3, 0); }
static JSValue dyn_gbc_ctor(JSContext *ctx, JSValueConst nt, int argc,
                            JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_gbc_class_id, 1, 1, 100, 3, 0); }
static JSValue dyn_xgbr_ctor(JSContext *ctx, JSValueConst nt, int argc,
                             JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_xgbr_class_id, 0, 1, 100, 6, 1); }
static JSValue dyn_xgbc_ctor(JSContext *ctx, JSValueConst nt, int argc,
                             JSValueConst *argv)
{ (void)nt; return dyn_forest_new(ctx, argc, argv, dyn_xgbc_class_id, 1, 1, 100, 6, 1); }

/* ---------- PCA (Jacobi eigendecomposition of the covariance) ----------------
 *
 * fit(): centre X, form the cols x cols covariance, diagonalise it with cyclic
 * Jacobi rotations, keep the leading eigenvectors as the components.
 *
 * Jacobi rather than a power method or a QR iteration because the matrix is
 * SYMMETRIC and small (cols x cols, never rows x rows): Jacobi is unconditionally
 * convergent on a symmetric matrix, needs no shifts or deflation, and produces
 * ALL eigenpairs at once with orthogonality maintained to machine precision by
 * construction -- each sweep is a product of exact rotations. The cost is
 * O(cols^3) per sweep, which is irrelevant next to the O(rows*cols^2) covariance
 * accumulation for any real dataset.
 *
 * The covariance is accumulated the same way as the linear-regression normal
 * equations: one axpy of the centred row into each row of the upper triangle, so
 * the inner loop is contiguous and vectorises.
 *
 * SIGN CONVENTION. An eigenvector is only defined up to sign, so a bare
 * eigensolver returns arbitrary signs and the same data can produce
 * component[0] = [0.7, 0.7] on one run and [-0.7, -0.7] on another (any change in
 * accumulation order is enough). Each component is therefore normalised so its
 * largest-magnitude entry is POSITIVE -- the same rule scikit-learn applies in
 * svd_flip -- which makes the output reproducible and diffable. */

typedef struct {
    int fitted;
    int whiten;
    size_t n_features;
    size_t n_components;
    double *mean;        /* n_features */
    double *comp;        /* n_components * n_features, row-major, unit rows */
    double *var;         /* n_components eigenvalues (explained variance) */
    double total_var;    /* sum of ALL eigenvalues, for the ratio */
} dyn_pca_t;

static JSClassID dyn_pca_class_id;

static void dyn_pca_dispose(void *native)
{
    dyn_pca_t *m = (dyn_pca_t *)native;
    if (m) {
        free(m->mean);
        free(m->comp);
        free(m->var);
        free(m);
    }
}

static const JSClassDef dyn_pca_class = {
    "PCA",
    .finalizer = dyn_res_finalizer,
};

/* new PCA(nComponents = 0, whiten = false); 0 means "all features". */
static JSValue dyn_pca_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_pca_t *m;
    int64_t nc = 0;

    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &nc, argv[0]))
            return JS_EXCEPTION;
        if (nc < 0)
            return JS_ThrowRangeError(ctx, "nComponents must not be negative");
    }
    m = (dyn_pca_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->n_components = (size_t)nc;
    if (argc > 1)
        m->whiten = JS_ToBool(ctx, argv[1]);
    return dyn_res_wrap(ctx, dyn_pca_class_id, m, dyn_pca_dispose);
}

#define DYN_JACOBI_SWEEPS 60

/* Cyclic Jacobi on the symmetric p x p matrix A (destroyed). Eigenvalues land on
 * A's diagonal, eigenvectors in the COLUMNS of V. */
static void dyn_jacobi(double *A, double *V, size_t p)
{
    size_t i, j, k, sweep;
    double prev_off = HUGE_VAL;

    for (i = 0; i < p; i++)
        for (j = 0; j < p; j++)
            V[i * p + j] = (i == j) ? 1.0 : 0.0;

    for (sweep = 0; sweep < DYN_JACOBI_SWEEPS; sweep++) {
        double off = 0.0;
        for (i = 0; i < p; i++)
            for (j = i + 1; j < p; j++)
                off += A[i * p + j] * A[i * p + j];
        /* The absolute 1e-300 test alone is unreachable for any p > 1: off
         * plateaus at the rounding floor (~p^2 * eps^2) far above it. A sweep
         * that stopped reducing off IS that floor -- per-element rotations at
         * 1e-16 change the answer by nothing measurable -- so stop there. */
        if (off <= 1e-300 || (sweep >= 2 && off >= prev_off))
            break;
        prev_off = off;
        for (i = 0; i < p; i++) {
            for (j = i + 1; j < p; j++) {
                double aij = A[i * p + j];
                double theta, t, c, s;
                if (fabs(aij) <= 1e-300)
                    continue;
                /* the standard stable rotation: solve for t = tan(phi) via the
                 * smaller root, which keeps |t| <= 1 and avoids cancellation */
                theta = (A[j * p + j] - A[i * p + i]) / (2.0 * aij);
                t = (theta >= 0.0 ? 1.0 : -1.0) /
                    (fabs(theta) + sqrt(theta * theta + 1.0));
                c = 1.0 / sqrt(t * t + 1.0);
                s = t * c;
                for (k = 0; k < p; k++) {
                    double aki = A[k * p + i], akj = A[k * p + j];
                    A[k * p + i] = c * aki - s * akj;
                    A[k * p + j] = s * aki + c * akj;
                }
                for (k = 0; k < p; k++) {
                    double aik = A[i * p + k], ajk = A[j * p + k];
                    A[i * p + k] = c * aik - s * ajk;
                    A[j * p + k] = s * aik + c * ajk;
                }
                for (k = 0; k < p; k++) {
                    double vki = V[k * p + i], vkj = V[k * p + j];
                    V[k * p + i] = c * vki - s * vkj;
                    V[k * p + j] = s * vki + c * vkj;
                }
            }
        }
    }
}

/* Fit: mean, covariance, eigendecomposition, component selection. */
static int dyn_pca_learn(JSContext *ctx, dyn_pca_t *m, const double *X,
                         size_t rows, size_t cols)
{
    double *mean = NULL, *cov = NULL, *V = NULL, *row = NULL;
    double *comp = NULL, *var = NULL;
    size_t *order = NULL;
    size_t want, i, j, a, b;
    double denom, total = 0.0;

    if (rows < 2) {
        JS_ThrowRangeError(ctx, "PCA needs at least two rows");
        return -1;
    }
    want = m->n_components ? m->n_components : cols;
    if (want > cols) {
        JS_ThrowRangeError(ctx,
            "nComponents (%u) exceeds the number of features (%u)",
            (unsigned)want, (unsigned)cols);
        return -1;
    }
    /* cov and V are cols x cols doubles; cols ~ 2^30.5 with rows == 2 wraps
     * cols*cols*8, and the wrapped malloc is a small VALID buffer the
     * covariance loops then overflow. cols*8 itself cannot wrap (the ingest
     * layer bounds rows*cols*8), so guarding the square guards the product. */
    if (cols > (SIZE_MAX / sizeof(double)) / cols) {
        JS_ThrowRangeError(ctx, "too many features for PCA");
        return -1;
    }
    mean = (double *)calloc(cols, sizeof(double));
    cov = (double *)calloc(cols * cols, sizeof(double));
    V = (double *)malloc(cols * cols * sizeof(double));
    row = (double *)malloc(cols * sizeof(double));
    comp = (double *)malloc(want * cols * sizeof(double));
    var = (double *)malloc(want * sizeof(double));
    order = (size_t *)malloc(cols * sizeof(size_t));
    if (!mean || !cov || !V || !row || !comp || !var || !order) {
        free(mean); free(cov); free(V); free(row);
        free(comp); free(var); free(order);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++)
        for (j = 0; j < cols; j++)
            mean[j] += X[i * cols + j];
    for (j = 0; j < cols; j++)
        mean[j] /= (double)rows;

    /* upper triangle of the covariance, one axpy of the centred row per column */
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        for (j = 0; j < cols; j++)
            row[j] = xi[j] - mean[j];
        for (a = 0; a < cols; a++)
            dyn_ml_axpy(cov + a * cols + a, row[a], row + a, cols - a);
    }
    denom = (double)(rows - 1);        /* sample covariance, ddof = 1 */
    for (a = 0; a < cols; a++)
        for (b = a; b < cols; b++) {
            double v = cov[a * cols + b] / denom;
            cov[a * cols + b] = v;
            cov[b * cols + a] = v;     /* mirror: Jacobi needs the full matrix */
        }
    /* finite inputs ~1e154 still overflow d*d to inf here; Jacobi would then
     * return NaN eigenvalues and every output would be silently NaN */
    for (a = 0; a < cols; a++)
        if (!isfinite(cov[a * cols + a])) {
            free(mean); free(cov); free(V); free(row);
            free(comp); free(var); free(order);
            JS_ThrowRangeError(ctx, "covariance overflowed; scale X before PCA");
            return -1;
        }

    dyn_jacobi(cov, V, cols);

    /* selection sort of the eigenvalues, descending: cols is small and this
     * keeps the eigenvalue/eigenvector pairing obvious */
    for (a = 0; a < cols; a++)
        order[a] = a;
    for (a = 0; a < cols; a++) {
        size_t best = a;
        for (b = a + 1; b < cols; b++)
            if (cov[order[b] * cols + order[b]] > cov[order[best] * cols + order[best]])
                best = b;
        if (best != a) {
            size_t t = order[a];
            order[a] = order[best];
            order[best] = t;
        }
    }
    for (a = 0; a < cols; a++) {
        double ev = cov[order[a] * cols + order[a]];
        total += (ev > 0.0) ? ev : 0.0;   /* a tiny negative is rounding, not signal */
    }
    for (a = 0; a < want; a++) {
        size_t src = order[a];
        double ev = cov[src * cols + src];
        double maxabs = 0.0, scale = 1.0;
        size_t maxj = 0;
        var[a] = (ev > 0.0) ? ev : 0.0;
        for (j = 0; j < cols; j++) {
            double v = fabs(V[j * cols + src]);
            if (v > maxabs) { maxabs = v; maxj = j; }
        }
        /* deterministic sign, then optional whitening */
        if (V[maxj * cols + src] < 0.0)
            scale = -1.0;
        if (m->whiten) {
            double sd = sqrt(var[a]);
            scale /= (sd > 0.0) ? sd : 1.0;
        }
        for (j = 0; j < cols; j++)
            comp[a * cols + j] = V[j * cols + src] * scale;
    }
    free(cov); free(V); free(row); free(order);
    free(m->mean); free(m->comp); free(m->var);
    m->mean = mean;
    m->comp = comp;
    m->var = var;
    m->n_features = cols;
    m->n_components = want;
    m->total_var = total;
    m->fitted = 1;
    return 0;
}

static JSValue dyn_pca_fit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). */
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "PCA"))
        return JS_EXCEPTION;
    dyn_pca_t *m;
    dyn_matrix_t mx = {0};
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (dyn_pca_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

/* transform / fitTransform / inverseTransform, shape-in shape-out. */
enum { DYN_PCA_FWD, DYN_PCA_FIT_FWD, DYN_PCA_INV };

static JSValue dyn_pca_apply(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int mode)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). This
     * covers fitTransform, which reaches PCA through here. */
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "PCA"))
        return JS_EXCEPTION;
    dyn_pca_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL, *row = NULL;
    size_t rows, i, j, a, in_cols, out_cols;
    int flat;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (mode == DYN_PCA_FIT_FWD) {
        /* fitTransform reaches dyn_pca_learn through here, so it needs the
         * same missing-data rejection every other fit path has */
        if (dyn_ml_check_finite(ctx, &mx, NULL)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
        if (dyn_pca_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "transform before fit");
    }
    in_cols = (mode == DYN_PCA_INV) ? m->n_components : m->n_features;
    out_cols = (mode == DYN_PCA_INV) ? m->n_features : m->n_components;
    if (mx.cols != in_cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u columns, expected %u",
                                 (unsigned)mx.cols, (unsigned)in_cols);
    }
    rows = mx.rows;
    flat = !mx.owned;
    /* inverse-transform can inflate rows*n_features past the ingest bound of
     * rows*n_components; the wrap is a heap overflow, guard it */
    if (rows > (SIZE_MAX / sizeof(double)) / out_cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "data too large for PCA transform");
    }
    out = (double *)malloc((rows * out_cols ? rows * out_cols : 1) * sizeof(double));
    row = (double *)malloc((m->n_features ? m->n_features : 1) * sizeof(double));
    if (!out || !row) {
        free(out); free(row);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (mode == DYN_PCA_INV) {
        for (i = 0; i < rows; i++) {
            const double *pi = mx.data + i * in_cols;
            double *oi = out + i * out_cols;
            for (j = 0; j < out_cols; j++)
                oi[j] = m->mean[j];
            for (a = 0; a < m->n_components; a++)
                dyn_ml_axpy(oi, pi[a], m->comp + a * m->n_features,
                            m->n_features);
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double *xi = mx.data + i * in_cols;
            double *oi = out + i * out_cols;
            for (j = 0; j < in_cols; j++)
                row[j] = xi[j] - m->mean[j];
            for (a = 0; a < out_cols; a++)
                oi[a] = dyn_ml_dot(row, m->comp + a * m->n_features,
                                   m->n_features);
        }
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    free(row);
    result = dyn_ml_matrix_to_js(ctx, out, rows, out_cols, flat);
    free(out);
    return result;
}

static JSValue dyn_pca_transform(JSContext *ctx, JSValueConst t, int argc,
                                 JSValueConst *argv)
{ return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FWD); }
static JSValue dyn_pca_fit_transform(JSContext *ctx, JSValueConst t, int argc,
                                     JSValueConst *argv)
{ return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FIT_FWD); }
static JSValue dyn_pca_inverse(JSContext *ctx, JSValueConst t, int argc,
                               JSValueConst *argv)
{ return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_INV); }

static JSValue dyn_pca_components(JSContext *ctx, JSValueConst this_val)
{
    dyn_pca_t *m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->comp, m->n_components, m->n_features, 0);
}

static JSValue dyn_pca_mean_get(JSContext *ctx, JSValueConst this_val)
{
    dyn_pca_t *m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->mean, m->n_features);
}

static JSValue dyn_pca_explained(JSContext *ctx, JSValueConst this_val)
{
    dyn_pca_t *m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->var, m->n_components);
}

static JSValue dyn_pca_explained_ratio(JSContext *ctx, JSValueConst this_val)
{
    dyn_pca_t *m = (dyn_pca_t *)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    double *tmp;
    size_t a;
    JSValue out;

    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    tmp = (double *)malloc(m->n_components * sizeof(double));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    for (a = 0; a < m->n_components; a++)
        tmp[a] = (m->total_var > 0.0) ? m->var[a] / m->total_var : 0.0;
    out = dyn_ml_doubles_to_js(ctx, tmp, m->n_components);
    free(tmp);
    return out;
}

static const JSCFunctionListEntry dyn_pca_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_pca_fit),
    JS_CFUNC_DEF("transform", 1, dyn_pca_transform),
    JS_CFUNC_DEF("fitTransform", 1, dyn_pca_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_pca_inverse),
    JS_CGETSET_DEF("components", dyn_pca_components, NULL),
    JS_CGETSET_DEF("mean", dyn_pca_mean_get, NULL),
    JS_CGETSET_DEF("explainedVariance", dyn_pca_explained, NULL),
    JS_CGETSET_DEF("explainedVarianceRatio", dyn_pca_explained_ratio, NULL),
};

/* ---------- GaussianNB -------------------------------------------------------
 *
 * Per class, per feature: a mean and a variance, plus the class prior. Prediction
 * scores each class with the log posterior and takes the argmax.
 *
 * Everything is done in LOG space. The likelihood is a product of `cols` Gaussian
 * densities, which for any real feature count underflows a double long before the
 * comparison happens -- computing it directly would score every class as exactly
 * 0 and return class 0 always. Summing logs cannot underflow.
 *
 * predictProba then needs to exponentiate, which is where the log-sum-exp shift
 * matters: subtracting the maximum log posterior before exp() keeps the largest
 * term at exactly 1.0 and every other in (0, 1], so no term overflows and the
 * normalisation is exact.
 *
 * Variances get `varSmoothing * (largest feature variance)` added, matching
 * scikit-learn: without it a feature that is constant within a class has zero
 * variance, and the density becomes a division by zero. */

typedef struct {
    int fitted;
    double var_smoothing;
    size_t n_features;
    size_t n_classes;
    double *classes;    /* n_classes label values, ascending */
    double *prior;      /* n_classes log priors */
    double *mean;       /* n_classes * n_features */
    double *var;        /* n_classes * n_features (already smoothed) */
    /* Derived at fit, so the SCORING loop contains no call and no division.
     * See dyn_nb_score: log(2*pi*var[j]) depends only on the model, so summing it
     * per row was `cols` libm calls per (row, class) -- and a call in the loop
     * body makes the loop unvectorisable no matter how it is written. */
    double *inv_var;    /* n_classes * n_features: 1/var */
    double *logdet;     /* n_classes: sum_j log(2*pi*var[c][j]) */
} dyn_nb_t;

static JSClassID dyn_nb_class_id;

static void dyn_nb_dispose(void *native)
{
    dyn_nb_t *m = (dyn_nb_t *)native;
    if (m) {
        free(m->classes);
        free(m->prior);
        free(m->mean);
        free(m->var);
        free(m->inv_var);
        free(m->logdet);
        free(m);
    }
}

static const JSClassDef dyn_nb_class = {
    "GaussianNB",
    .finalizer = dyn_res_finalizer,
};

/* new GaussianNB(varSmoothing = 1e-9) */
static JSValue dyn_nb_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    dyn_nb_t *m;
    double vs = 1e-9;

    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &vs, argv[0]))
            return JS_EXCEPTION;
        if (!(vs >= 0.0))
            return JS_ThrowRangeError(ctx, "varSmoothing must not be negative");
    }
    m = (dyn_nb_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->var_smoothing = vs;
    return dyn_res_wrap(ctx, dyn_nb_class_id, m, dyn_nb_dispose);
}

static int dyn_nb_learn(JSContext *ctx, dyn_nb_t *m, const double *X,
                        const double *y, size_t rows, size_t cols,
                        const double *w)
{
    double *classes = NULL, *prior = NULL, *mean = NULL, *var = NULL;
    double *inv_var = NULL, *logdet = NULL;
    double *count = NULL;    /* weight per class; the row count when unweighted */
    size_t nc, c, i, j;
    double maxvar = 0.0, eps, wtot = 0.0;

    nc = dyn_ml_classes(ctx, y, rows, &classes);
    if (nc == 0)
        return -1;
    prior = (double *)calloc(nc, sizeof(double));
    mean = (double *)calloc(nc * cols, sizeof(double));
    var = (double *)calloc(nc * cols, sizeof(double));
    inv_var = (double *)calloc(nc * cols, sizeof(double));
    logdet = (double *)calloc(nc, sizeof(double));
    count = (double *)calloc(nc, sizeof(double));
    if (!prior || !mean || !var || !inv_var || !logdet || !count) {
        free(classes); free(prior); free(mean); free(var);
        free(inv_var); free(logdet); free(count);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        double wi = w ? w[i] : 1.0;
        for (c = 0; c < nc; c++)
            if (classes[c] == y[i])
                break;
        /* unreachable while y is finite-checked upstream; a guard against a
         * future path that stops doing so */
        if (c == nc) {
            free(classes); free(prior); free(mean); free(var);
            free(inv_var); free(logdet); free(count);
            JS_ThrowInternalError(ctx, "NaiveBayes: label outside class table");
            return -1;
        }
        count[c] += wi;
        wtot += wi;
        for (j = 0; j < cols; j++) {
            mean[c * cols + j] += wi * xi[j];
            var[c * cols + j] += wi * xi[j] * xi[j];
        }
    }
    for (c = 0; c < nc; c++) {
        /* A class every one of whose rows weighs zero has no moments; its
         * prior is -inf, so it can never win a comparison. */
        if (count[c] <= 0.0) {
            for (j = 0; j < cols; j++) {
                mean[c * cols + j] = 0.0;
                var[c * cols + j] = 0.0;
            }
            prior[c] = -INFINITY;
            continue;
        }
        for (j = 0; j < cols; j++) {
            double mu = mean[c * cols + j] / count[c];
            double v = var[c * cols + j] / count[c] - mu * mu;
            /* finite inputs ~1e154 still overflow x*x to inf, and inf-inf is
             * NaN, which both clamps below pass through untouched; a NaN
             * variance silently classifies every row as classes[0]. Refuse. */
            if (!isfinite(v)) {
                free(classes); free(prior); free(mean); free(var);
                free(inv_var); free(logdet); free(count);
                JS_ThrowRangeError(ctx,
                    "variance overflowed; scale X before GaussianNB");
                return -1;
            }
            if (v < 0.0)
                v = 0.0;              /* E[x^2]-mu^2 rounding on a constant feature */
            mean[c * cols + j] = mu;
            var[c * cols + j] = v;
            if (v > maxvar)
                maxvar = v;
        }
        prior[c] = log(count[c] / wtot);
    }
    /* smoothing floor relative to the largest variance in the data; when every
     * feature is constant fall back to an absolute floor so it is never 0 */
    eps = m->var_smoothing * maxvar;
    if (!(eps > 0.0))
        eps = (m->var_smoothing > 0.0) ? m->var_smoothing : 0.0;
    for (c = 0; c < nc * cols; c++) {
        var[c] += eps;
        if (var[c] <= 0.0)
            var[c] = 1e-300;          /* a density, never a division by zero */
    }
    /* The whole point: every transcendental and every division the scoring loop
     * would otherwise repeat per row is done here, once. */
    for (c = 0; c < nc; c++) {
        double ld = 0.0;
        for (j = 0; j < cols; j++) {
            double v = var[c * cols + j];
            inv_var[c * cols + j] = 1.0 / v;
            ld += log(6.283185307179586476925286766559 * v);
        }
        logdet[c] = ld;
    }
    free(count);
    free(m->classes); free(m->prior); free(m->mean); free(m->var);
    free(m->inv_var); free(m->logdet);
    m->classes = classes;
    m->prior = prior;
    m->mean = mean;
    m->var = var;
    m->inv_var = inv_var;
    m->logdet = logdet;
    m->n_features = cols;
    m->n_classes = nc;
    m->fitted = 1;
    return 0;
}

static JSValue dyn_nb_fit(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    dyn_nb_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL, *w = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_weights(ctx, argc, argv, 2, mx.rows, &w)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    m = (dyn_nb_t *)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y); free(w);
        return JS_EXCEPTION;
    }
    if (dyn_nb_learn(ctx, m, mx.data, y, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx); free(y); free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    free(w);
    return JS_DupValue(ctx, this_val);
}

/* log P(class c | x) up to the shared evidence term.
 *
 * The Mahalanobis term is the only thing left in the loop: log(2*pi*var[j]) is
 * model-only and was hoisted into logdet[c] at fit, and the division by var[j] is
 * a multiply by the precomputed reciprocal. What remains is a pure arithmetic
 * reduction over two contiguous arrays, written with four independent
 * accumulators so it VECTORISES -- verified in the generated assembly (fmla.2d,
 * zero `bl _log`). Before this, the loop made `cols` libm calls per (row, class),
 * and a call in the body makes a loop unvectorisable however it is written. */
static double dyn_nb_score(const dyn_nb_t *m, const double *x, size_t c)
{
    const double *mu = m->mean + c * m->n_features;
    const double *iv = m->inv_var + c * m->n_features;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, d;
    size_t n = m->n_features, j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        d = x[j] - mu[j];         s0 += d * d * iv[j];
        d = x[j + 1] - mu[j + 1]; s1 += d * d * iv[j + 1];
        d = x[j + 2] - mu[j + 2]; s2 += d * d * iv[j + 2];
        d = x[j + 3] - mu[j + 3]; s3 += d * d * iv[j + 3];
    }
#endif
    for (; j < n; j++) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
    }
    return m->prior[c] - 0.5 * (m->logdet[c] + ((s0 + s1) + (s2 + s3)));
}

static JSValue dyn_nb_predict_impl(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int proba)
{
    dyn_nb_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL, *sc = NULL;
    size_t rows, i, c, nc;
    int flat;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    /* a NaN row cannot beat -HUGE_VAL, so it would silently classify as the
     * first class; predict is held to the same missing-data rule as fit */
    if (dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_nb_t *)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->n_features);
    }
    rows = mx.rows;
    nc = m->n_classes;
    flat = !mx.owned;
    out = (double *)malloc((proba ? rows * nc : rows) * sizeof(double));
    sc = (double *)malloc(nc * sizeof(double));
    if (!out || !sc) {
        free(out); free(sc);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        double best = -HUGE_VAL;
        size_t bestc = 0;
        for (c = 0; c < nc; c++) {
            sc[c] = dyn_nb_score(m, xi, c);
            if (sc[c] > best) {
                best = sc[c];
                bestc = c;
            }
        }
        if (!proba) {
            out[i] = m->classes[bestc];
        } else {
            /* log-sum-exp about the maximum: the largest term becomes exactly
             * 1.0, so nothing overflows and the sum cannot be 0 */
            double sum = 0.0;
            for (c = 0; c < nc; c++) {
                out[i * nc + c] = exp(sc[c] - best);
                sum += out[i * nc + c];
            }
            for (c = 0; c < nc; c++)
                out[i * nc + c] /= sum;
        }
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    free(sc);
    result = proba ? dyn_ml_matrix_to_js(ctx, out, rows, nc, flat)
                   : dyn_ml_doubles_to_js(ctx, out, rows);
    free(out);
    return result;
}

static JSValue dyn_nb_predict(JSContext *ctx, JSValueConst t, int argc,
                              JSValueConst *argv)
{ return dyn_nb_predict_impl(ctx, t, argc, argv, 0); }
static JSValue dyn_nb_predict_proba(JSContext *ctx, JSValueConst t, int argc,
                                    JSValueConst *argv)
{ return dyn_nb_predict_impl(ctx, t, argc, argv, 1); }

static JSValue dyn_nb_classes_get(JSContext *ctx, JSValueConst this_val)
{
    dyn_nb_t *m = (dyn_nb_t *)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

static const JSCFunctionListEntry dyn_nb_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_nb_fit),
    JS_CFUNC_DEF("predict", 1, dyn_nb_predict),
    JS_CFUNC_DEF("predictProba", 1, dyn_nb_predict_proba),
    JS_CGETSET_DEF("classes", dyn_nb_classes_get, NULL),
};

/* ---------- KN: classifier + regressor ------------------------------
 *
 * Lazy learner: fit() just keeps a copy of the training set, predict() scans it.
 * The whole cost is the distance scan, which is dyn_ml_sqdist -- the vectorised
 * multi-accumulator loop -- run rows_train times per query.
 *
 * Distances stay SQUARED throughout. The k-nearest set is identical either way
 * (sqrt is monotone), so the only sqrt calls are the k per query that distance
 * weighting actually needs, instead of one per training point.
 *
 * Selection is insertion into a sorted k-array rather than a heap: k is small (5
 * by default), the array is contiguous and branch-predictable, and the common
 * case after the first few candidates is a single compare against the current
 * worst that fails and moves on. */

typedef struct {
    int fitted;
    int regressor;      /* 0: majority vote, 1: mean of neighbours */
    int weighted;       /* 0: uniform, 1: weight by 1/distance */
    size_t k;
    size_t rows, cols;
    double *X;          /* rows*cols training features */
    double *y;          /* rows targets/labels */
} dyn_knn_t;

static JSClassID dyn_knn_clf_class_id;
static JSClassID dyn_knn_reg_class_id;

static void dyn_knn_dispose(void *native)
{
    dyn_knn_t *m = (dyn_knn_t *)native;
    if (m) {
        free(m->X);
        free(m->y);
        free(m);
    }
}

static const JSClassDef dyn_knn_clf_class = {
    "KNClassifier",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_knn_reg_class = {
    "KNRegressor",
    .finalizer = dyn_res_finalizer,
};

/* new KN*(k = 5, weights = "uniform"). `weights` is "uniform" or
 * "distance"; anything else is a TypeError rather than a silent default, since a
 * typo there would quietly change the model. */
static JSValue dyn_knn_new(JSContext *ctx, int argc, JSValueConst *argv,
                           JSClassID class_id, int regressor)
{
    dyn_knn_t *m;
    int64_t k = 5;
    int weighted = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k <= 0)
            return JS_ThrowRangeError(ctx, "k must be positive");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char *w = JS_ToCString(ctx, argv[1]);
        if (!w)
            return JS_EXCEPTION;
        if (!strcmp(w, "distance"))
            weighted = 1;
        else if (strcmp(w, "uniform")) {
            JS_FreeCString(ctx, w);
            return JS_ThrowTypeError(ctx,
                "weights must be \"uniform\" or \"distance\"");
        }
        JS_FreeCString(ctx, w);
    }
    m = (dyn_knn_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = (size_t)k;
    m->weighted = weighted;
    m->regressor = regressor;
    return dyn_res_wrap(ctx, class_id, m, dyn_knn_dispose);
}

static JSValue dyn_knn_clf_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    (void)new_target;
    return dyn_knn_new(ctx, argc, argv, dyn_knn_clf_class_id, 0);
}

static JSValue dyn_knn_reg_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    (void)new_target;
    return dyn_knn_new(ctx, argc, argv, dyn_knn_reg_class_id, 1);
}

/* fit(X, y) / fit(X, y, rows, cols) -> this. Takes an OWNED copy of X even in
 * the flat form: the model outlives the call, so it must not hold an alias into
 * a JS ArrayBuffer that the program can detach or resize afterwards. */
static JSValue dyn_knn_fit_impl(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, JSClassID class_id)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). */
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "KNClassifier/KNRegressor"))
        return JS_EXCEPTION;
    dyn_knn_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL, *Xcopy;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_knn_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    if (mx.rows < m->k) {
        dyn_matrix_free(&mx); free(y);
        return JS_ThrowRangeError(ctx,
            "fit needs at least k rows (k = %u, rows = %u)",
            (unsigned)m->k, (unsigned)mx.rows);
    }
    Xcopy = (double *)malloc(mx.rows * mx.cols * sizeof(double));
    if (!Xcopy) {
        dyn_matrix_free(&mx); free(y);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(Xcopy, mx.data, mx.rows * mx.cols * sizeof(double));
    free(m->X);
    free(m->y);
    m->X = Xcopy;
    m->y = y;          /* already an owned buffer from the ingest */
    m->rows = mx.rows;
    m->cols = mx.cols;
    m->fitted = 1;
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

/* Fill nd[0..k) / ni[0..k) with the k smallest squared distances from `q` to the
 * training set, ascending. Requires rows >= k (checked at fit). */
static void dyn_knn_nearest(const dyn_knn_t *m, const double *q, double *nd,
                            size_t *ni)
{
    size_t k = m->k, cols = m->cols, i, j, p;

    for (j = 0; j < k; j++) {
        nd[j] = HUGE_VAL;
        ni[j] = 0;
    }
    for (i = 0; i < m->rows; i++) {
        double d = dyn_ml_sqdist(q, m->X + i * cols, cols);
        if (d >= nd[k - 1])
            continue;      /* the usual case once the array is warm */
        for (p = k - 1; p > 0 && nd[p - 1] > d; p--) {
            nd[p] = nd[p - 1];
            ni[p] = ni[p - 1];
        }
        nd[p] = d;
        ni[p] = i;
    }
}

/* predict(X) / predict(X, rows, cols) -> Array<number> */
static JSValue dyn_knn_predict_impl(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    JSClassID class_id)
{
    dyn_knn_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL, *nd = NULL;
    size_t *ni = NULL;
    size_t rows, i, a, b, k;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    /* a NaN query distance is never < a real one, so NaN rows would vote
     * garbage neighbours in; predict is held to the fit-time rule */
    if (dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_knn_t *)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->cols);
    }
    rows = mx.rows;
    k = m->k;
    out = (double *)malloc(rows * sizeof(double));
    nd = (double *)malloc(k * sizeof(double));
    ni = (size_t *)malloc(k * sizeof(size_t));
    if (!out || !nd || !ni) {
        free(out); free(nd); free(ni);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *q = mx.data + i * mx.cols;
        dyn_knn_nearest(m, q, nd, ni);
        if (m->regressor) {
            double num = 0.0, den = 0.0;
            for (a = 0; a < k; a++) {
                double w = 1.0;
                if (m->weighted) {
                    double dist = sqrt(nd[a]);
                    /* an exact hit would be an infinite weight: the spec answer
                     * is that neighbour's own target, so take it and stop */
                    if (dist == 0.0) {
                        num = m->y[ni[a]];
                        den = 1.0;
                        break;
                    }
                    w = 1.0 / dist;
                }
                num += w * m->y[ni[a]];
                den += w;
            }
            out[i] = num / den;
        } else {
            /* Weighted majority vote. k is small, so counting by rescanning the
             * k neighbours is cheaper than building a map. Ties go to the
             * smaller label, which makes the result deterministic. */
            double best_label = m->y[ni[0]], best_score = -1.0;
            for (a = 0; a < k; a++) {
                double lab = m->y[ni[a]], score = 0.0;
                for (b = 0; b < k; b++) {
                    if (m->y[ni[b]] != lab)
                        continue;
                    if (!m->weighted) {
                        score += 1.0;
                    } else {
                        double dist = sqrt(nd[b]);
                        score += (dist == 0.0) ? HUGE_VAL : 1.0 / dist;
                    }
                }
                if (score > best_score || (score == best_score && lab < best_label)) {
                    best_score = score;
                    best_label = lab;
                }
            }
            out[i] = best_label;
        }
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    free(nd);
    free(ni);
    result = dyn_ml_doubles_to_js(ctx, out, rows);
    free(out);
    return result;
}

#define DYN_KNN_METHODS(prefix, class_id_var)                                  \
static JSValue prefix##_fit(JSContext *ctx, JSValueConst t, int argc,           \
                            JSValueConst *argv)                                \
{ return dyn_knn_fit_impl(ctx, t, argc, argv, class_id_var); }                  \
static JSValue prefix##_predict(JSContext *ctx, JSValueConst t, int argc,       \
                                JSValueConst *argv)                            \
{ return dyn_knn_predict_impl(ctx, t, argc, argv, class_id_var); }

DYN_KNN_METHODS(dyn_knn_clf, dyn_knn_clf_class_id)
DYN_KNN_METHODS(dyn_knn_reg, dyn_knn_reg_class_id)

static const JSCFunctionListEntry dyn_knn_clf_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_clf_fit),
    JS_CFUNC_DEF("predict", 1, dyn_knn_clf_predict),
};

static const JSCFunctionListEntry dyn_knn_reg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_reg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_knn_reg_predict),
};

/* ---------- DBScan -----------------------------------------------------------
 *
 * Density-based clustering: a point with at least minPts neighbours within eps
 * (counting itself) is a CORE point; cores that are within eps of each other are
 * transitively one cluster; a non-core point within eps of a core joins it as a
 * BORDER point; anything else is NOISE, labelled -1.
 *
 * A grid index over eps makes a region query scan only the 3^cols cells
 * around the query (dyn_dbscan_grid_*), which removes the O(rows) factor on
 * low-dimensional data; for wide data, where the cell fan-out would lose,
 * the query falls back to a linear scan -- the same trade scikit-learn makes.
 * Memory stays O(rows): the neighbour lists are recomputed on demand into
 * one scratch buffer per query rather than materialised as an O(rows^2)
 * matrix, which is what would actually make large inputs fail.
 *
 * Comparisons are made against eps SQUARED so the scan needs no sqrt, and the
 * distance is computed directly by dyn_ml_sqdist: the ||a||^2 - 2ab + ||b||^2
 * identity would be catastrophically cancelling exactly at the eps boundary,
 * where the answer decides cluster membership. */

/* Ordered largest-first: `int fitted` in front of the doubles cost 8 bytes of
   interior padding (56 -> 48). */
typedef struct {
    double eps2;        /* eps^2, so the scan never needs sqrt */
    double eps;         /* kept for the getter */
    size_t min_pts;
    size_t rows;
    int *labels;        /* rows entries; -1 = noise */
    int fitted;
    int n_clusters;
} dyn_dbscan_t;

_Static_assert(sizeof(dyn_dbscan_t) <= 48,
               "dyn_dbscan_t regained padding: reorder largest-first");

static JSClassID dyn_dbscan_class_id;

static void dyn_dbscan_dispose(void *native)
{
    dyn_dbscan_t *m = (dyn_dbscan_t *)native;
    if (m) {
        free(m->labels);
        free(m);
    }
}

static const JSClassDef dyn_dbscan_class = {
    "DBScan",
    .finalizer = dyn_res_finalizer,
};

/* new DBScan(eps = 0.5, minPts = 5) */
static JSValue dyn_dbscan_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_dbscan_t *m;
    double eps = 0.5;
    int64_t min_pts = 5;

    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &eps, argv[0]))
            return JS_EXCEPTION;
        if (!(eps > 0.0))
            return JS_ThrowRangeError(ctx, "eps must be positive");
        if (!isfinite(eps) || !isfinite(eps * eps))
            return JS_ThrowRangeError(ctx,
                "eps must be finite and its square must not overflow");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &min_pts, argv[1]))
            return JS_EXCEPTION;
        if (min_pts < 1)
            return JS_ThrowRangeError(ctx, "minPts must be at least 1");
    }
    m = (dyn_dbscan_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->eps = eps;
    m->eps2 = eps * eps;
    m->min_pts = (size_t)min_pts;
    return dyn_res_wrap(ctx, dyn_dbscan_class_id, m, dyn_dbscan_dispose);
}

/* Indices within eps of point `p` (inclusive of p itself) into out[]; count. */
/* The grid index over eps: each point hashes to a cell; a region query then
 * scans only the 3^cols cells around the query's own cell instead of all rows.
 * Cell size is exactly eps, so any point within eps lies in the same or a
 * coordinate-adjacent cell, and the sqdist filter keeps the exact same result
 * set as the linear scan. Used when 3^cols is much smaller than rows; the
 * linear scan stays for wide data where the cell fan-out would lose. */
#define DYN_DBSCAN_MAX_GRID_COLS 6

typedef struct {
    size_t *head;     /* hash bucket -> first point index, SIZE_MAX empty */
    size_t *next;     /* per-point chain */
    size_t cap;       /* hash table size, power of two */
    size_t cols;
    double inv_eps;
} dyn_dbscan_grid_t;

static uint64_t dyn_dbscan_cell_hash(const int64_t *c, size_t cols)
{
    uint64_t h = 1469598103934665603ULL;
    size_t d;
    for (d = 0; d < cols; d++) {
        h ^= (uint64_t)c[d];
        h *= 1099511628211ULL;
    }
    return h;
}

static int64_t dyn_dbscan_coord(double x, double inv_eps)
{
    double cd = floor(x * inv_eps);
    /* clamp before the cast: x*inv_eps can be +-inf for tiny eps and huge
     * coordinates, and (int64_t)inf is UB; clamping far points into one
     * bucket degrades to a scan, never to UB */
    if (cd > 8.0e18)
        cd = 8.0e18;
    else if (cd < -8.0e18)
        cd = -8.0e18;
    return (int64_t)cd;
}

static int dyn_dbscan_grid_build(JSContext *ctx, dyn_dbscan_grid_t *g,
                                 const double *X, size_t rows, size_t cols,
                                 double eps)
{
    size_t cap = 16, i, d;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];

    while (cap < rows * 2 && cap <= SIZE_MAX / 2)
        cap *= 2;
    g->head = (size_t *)malloc(cap * sizeof(size_t));
    g->next = (size_t *)malloc(rows * sizeof(size_t));
    if (!g->head || !g->next) {
        free(g->head); free(g->next);
        g->head = NULL; g->next = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < cap; i++)
        g->head[i] = SIZE_MAX;
    g->cap = cap;
    g->cols = cols;
    g->inv_eps = 1.0 / eps;
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * cols;
        for (d = 0; d < cols; d++)
            c[d] = dyn_dbscan_coord(xi[d], g->inv_eps);
        {
            size_t b = (size_t)dyn_dbscan_cell_hash(c, cols) & (cap - 1);
            g->next[i] = g->head[b];
            g->head[b] = i;
        }
    }
    return 0;
}

static size_t dyn_dbscan_grid_region(const dyn_dbscan_grid_t *g,
                                     const double *X, size_t p, double eps2,
                                     size_t *out)
{
    const double *xp = X + p * g->cols;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];
    int64_t off[DYN_DBSCAN_MAX_GRID_COLS] = {0};
    size_t n = 0, d, k;

    for (d = 0; d < g->cols; d++)
        c[d] = dyn_dbscan_coord(xp[d], g->inv_eps);
    for (;;) {                        /* all 3^cols neighbour cells */
        int64_t cc[DYN_DBSCAN_MAX_GRID_COLS];
        for (d = 0; d < g->cols; d++)
            cc[d] = c[d] + off[d] - 1;
        {
            size_t b = (size_t)dyn_dbscan_cell_hash(cc, g->cols) & (g->cap - 1);
            for (k = g->head[b]; k != SIZE_MAX; k = g->next[k])
                if (dyn_ml_sqdist(xp, X + k * g->cols, g->cols) <= eps2)
                    out[n++] = k;
        }
        for (d = 0; d < g->cols; d++) {
            if (++off[d] < 3)
                break;
            off[d] = 0;
        }
        if (d == g->cols)
            break;
    }
    return n;
}

static size_t dyn_dbscan_region(const double *X, size_t rows, size_t cols,
                                size_t p, double eps2, size_t *out)
{
    const double *xp = X + p * cols;
    size_t n = 0, i;
    for (i = 0; i < rows; i++)
        if (dyn_ml_sqdist(xp, X + i * cols, cols) <= eps2)
            out[n++] = i;
    return n;
}

/* Label every point. `labels` is rows entries: -1 noise, else a 0-based cluster
 * id. Returns the cluster count, or -1 on OOM (throwing). */
static int dyn_dbscan_run(JSContext *ctx, dyn_dbscan_t *m, const double *X,
                          size_t rows, size_t cols, int *labels)
{
    size_t *neigh = NULL, *queue = NULL, *seed = NULL;
    unsigned char *queued = NULL;
    dyn_dbscan_grid_t grid;
    size_t i, qn, qi, nn, j, sn;
    int cluster = 0, have_grid;

    neigh = (size_t *)malloc(rows * sizeof(size_t));
    seed = (size_t *)malloc(rows * sizeof(size_t));
    queue = (size_t *)malloc(rows * sizeof(size_t));
    queued = (unsigned char *)calloc(rows, 1);
    if (!neigh || !seed || !queue || !queued) {
        free(neigh); free(seed); free(queue); free(queued);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    /* The grid pays when the 3^cols cell fan-out is a small fraction of the
     * row count; on wide or small data the linear scan wins and is used. */
    have_grid = (cols <= DYN_DBSCAN_MAX_GRID_COLS &&
                 rows >= (size_t)1 << (cols * 2));
    memset(&grid, 0, sizeof(grid));
    if (have_grid &&
        dyn_dbscan_grid_build(ctx, &grid, X, rows, cols, m->eps)) {
        free(neigh); free(seed); free(queue); free(queued);
        return -1;
    }
    for (i = 0; i < rows; i++)
        labels[i] = -1;                       /* noise until proven otherwise */

    for (i = 0; i < rows; i++) {
        if (labels[i] != -1 || queued[i])
            continue;                          /* already assigned this round */
        nn = have_grid
                 ? dyn_dbscan_grid_region(&grid, X, i, m->eps2, neigh)
                 : dyn_dbscan_region(X, rows, cols, i, m->eps2, neigh);
        if (nn < m->min_pts)
            continue;                          /* not a core point: leave as noise */
        /* new cluster, expanded breadth-first from this core point */
        labels[i] = cluster;
        qn = 0;
        for (j = 0; j < nn; j++) {
            size_t q = neigh[j];
            if (q == i || queued[q])
                continue;
            queued[q] = 1;
            queue[qn++] = q;
        }
        for (qi = 0; qi < qn; qi++) {
            size_t q = queue[qi];
            /* A point already in a cluster keeps its first assignment -- a border
             * point reachable from two clusters belongs to whichever claimed it
             * first, which is the standard resolution of that ambiguity. */
            if (labels[q] == -1)
                labels[q] = cluster;
            sn = have_grid
                     ? dyn_dbscan_grid_region(&grid, X, q, m->eps2, seed)
                     : dyn_dbscan_region(X, rows, cols, q, m->eps2, seed);
            if (sn < m->min_pts)
                continue;                      /* border point: do not expand */
            for (j = 0; j < sn; j++) {
                size_t s = seed[j];
                if (queued[s] || labels[s] != -1)
                    continue;
                queued[s] = 1;
                queue[qn++] = s;               /* bounded by rows: each id once */
            }
        }
        cluster++;
    }
    free(grid.head);
    free(grid.next);
    free(neigh);
    free(seed);
    free(queue);
    free(queued);
    return cluster;
}

/* fit(X) / fit(X, rows, cols) -> this */
static JSValue dyn_dbscan_fit(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). */
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "DBScan"))
        return JS_EXCEPTION;
    dyn_dbscan_t *m;
    dyn_matrix_t mx = {0};
    int *labels;
    int nc;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_dbscan_t *)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    labels = (int *)malloc(mx.rows * sizeof(int));
    if (!labels) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    nc = dyn_dbscan_run(ctx, m, mx.data, mx.rows, mx.cols, labels);
    dyn_matrix_free(&mx);
    if (nc < 0) {
        free(labels);
        return JS_EXCEPTION;
    }
    free(m->labels);
    m->labels = labels;
    m->rows = mx.rows;
    m->n_clusters = nc;
    m->fitted = 1;
    return JS_DupValue(ctx, this_val);
}

/* labels -> Array<number>, -1 for noise. Empty before fit. */
static JSValue dyn_dbscan_labels(JSContext *ctx, JSValueConst this_val)
{
    dyn_dbscan_t *m =
        (dyn_dbscan_t *)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_ints_to_js(ctx, m->labels, m->rows);
}

static JSValue dyn_dbscan_nclusters(JSContext *ctx, JSValueConst this_val)
{
    dyn_dbscan_t *m =
        (dyn_dbscan_t *)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, m->fitted ? m->n_clusters : 0);
}

static JSValue dyn_dbscan_eps(JSContext *ctx, JSValueConst this_val)
{
    dyn_dbscan_t *m =
        (dyn_dbscan_t *)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->eps);
}

static const JSCFunctionListEntry dyn_dbscan_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_dbscan_fit),
    JS_CGETSET_DEF("labels", dyn_dbscan_labels, NULL),
    JS_CGETSET_DEF("nClusters", dyn_dbscan_nclusters, NULL),
    JS_CGETSET_DEF("eps", dyn_dbscan_eps, NULL),
};

/* ---------- preprocessing: StandardScaler / MinMaxScaler --------------------
 *
 * Both are per-feature (column) statistics, and X is ROW-major, so a column is
 * strided and no kernel applies to it directly. Both therefore accumulate a
 * VECTOR of per-column statistics one row at a time: `acc[j] += xi[j]` over a
 * contiguous row, which is element-wise and vectorises inline (see the loop-shape
 * note at the top of this file). One pass over X computes every column's sum and
 * sum of squares together, so the whole fit is O(rows*cols) with a single
 * traversal and no transpose.
 *
 * A zero-variance / zero-range column scales by 1 instead of dividing by zero,
 * matching scikit-learn: such a column carries no information and must not
 * become NaN or Inf and poison every downstream model. */

typedef struct {
    int fitted;
    int minmax;        /* 0: z-score (centre/scale), 1: min-max */
    size_t n_features;
    double *centre;    /* mean, or data minimum */
    double *scale;     /* 1/std, or 1/(max-min) -- always a MULTIPLIER */
    double *spread;    /* std, or (max-min) -- kept for the getters + inverse */
} dyn_scaler_t;

static JSClassID dyn_stdscaler_class_id;
static JSClassID dyn_minmax_class_id;

static void dyn_scaler_dispose(void *native)
{
    dyn_scaler_t *s = (dyn_scaler_t *)native;
    if (s) {
        free(s->centre);
        free(s->scale);
        free(s->spread);
        free(s);
    }
}

static const JSClassDef dyn_stdscaler_class = {
    "StandardScaler",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_minmax_class = {
    "MinMaxScaler",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_scaler_new(JSContext *ctx, JSClassID class_id, int minmax)
{
    dyn_scaler_t *s = (dyn_scaler_t *)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->minmax = minmax;
    return dyn_res_wrap(ctx, class_id, s, dyn_scaler_dispose);
}

static JSValue dyn_stdscaler_ctor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return dyn_scaler_new(ctx, dyn_stdscaler_class_id, 0);
}

static JSValue dyn_minmax_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return dyn_scaler_new(ctx, dyn_minmax_class_id, 1);
}

/* Compute the fit statistics into freshly allocated buffers on `s`. */
static int dyn_scaler_learn(JSContext *ctx, dyn_scaler_t *s, const double *X,
                            size_t rows, size_t cols, const double *w)
{
    double *centre, *scale, *spread, *sumsq = NULL;
    size_t i, j;

    centre = (double *)calloc(cols, sizeof(double));
    scale = (double *)malloc(cols * sizeof(double));
    spread = (double *)malloc(cols * sizeof(double));
    if (!s->minmax)
        sumsq = (double *)calloc(cols, sizeof(double));
    if (!centre || !scale || !spread || (!s->minmax && !sumsq)) {
        free(centre); free(scale); free(spread); free(sumsq);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (s->minmax) {
        /* centre = per-column min, spread = max - min */
        for (j = 0; j < cols; j++) {
            centre[j] = X[j];
            spread[j] = X[j];   /* running max until the subtraction below */
        }
        for (i = 1; i < rows; i++) {
            const double *xi = X + i * cols;
            for (j = 0; j < cols; j++) {
                if (xi[j] < centre[j]) centre[j] = xi[j];
                if (xi[j] > spread[j]) spread[j] = xi[j];
            }
        }
        for (j = 0; j < cols; j++) {
            double range = spread[j] - centre[j];
            spread[j] = range;
            scale[j] = (range > 0.0) ? 1.0 / range : 1.0;
        }
    } else {
        /* centre = mean, spread = population std (ddof = 0). TWO PASSES:
         * E[x^2] - mean^2 cancels to 0 for large-mean columns (epoch-ms
         * timestamps), silently snapping scale to 1.0; deviations do not. */
        double wtot = (double)rows;
        size_t i2;
        if (w) {
            wtot = 0.0;
            for (i = 0; i < rows; i++) {
                const double *xi = X + i * cols;
                double wi = w[i];
                wtot += wi;
                for (j = 0; j < cols; j++)
                    centre[j] += wi * xi[j];
            }
        } else {
            for (i = 0; i < rows; i++) {
                const double *xi = X + i * cols;
                for (j = 0; j < cols; j++)
                    centre[j] += xi[j];
            }
        }
        for (j = 0; j < cols; j++)
            centre[j] /= wtot;          /* centre is the mean from here on */
        if (w) {
            for (i2 = 0; i2 < rows; i2++) {
                const double *xi = X + i2 * cols;
                double wi = w[i2];
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - centre[j];
                    sumsq[j] += wi * d * d;
                }
            }
        } else {
            for (i2 = 0; i2 < rows; i2++) {
                const double *xi = X + i2 * cols;
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - centre[j];
                    sumsq[j] += d * d;
                }
            }
        }
        for (j = 0; j < cols; j++) {
            /* large-magnitude-but-finite columns still overflow d*d to inf;
             * a NaN variance would silently emit NaN transforms. Refuse. */
            double var = sumsq[j] / wtot;
            double sd;
            if (!isfinite(var)) {
                free(centre); free(scale); free(spread); free(sumsq);
                JS_ThrowRangeError(ctx,
                    "variance overflowed; scale X before StandardScaler");
                return -1;
            }
            if (var < 0.0)
                var = 0.0;
            sd = sqrt(var);
            /* sklearn's scale_ convention: a constant column reports 1.0, not
             * 0 (the transform divides by the same scale either way, but the
             * observable differs, and the differential pins it) */
            spread[j] = (sd > 0.0) ? sd : 1.0;
            scale[j] = (sd > 0.0) ? 1.0 / sd : 1.0;
        }
        free(sumsq);
    }
    free(s->centre); free(s->scale); free(s->spread);
    s->centre = centre;
    s->scale = scale;
    s->spread = spread;
    s->n_features = cols;
    s->fitted = 1;
    return 0;
}

/* fit(X) / fit(X, rows, cols) -> this */
static JSValue dyn_scaler_fit_impl(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   JSClassID class_id)
{
    dyn_scaler_t *s;
    dyn_matrix_t mx = {0};
    double *w = NULL;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    int minmax;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_weights(ctx, argc, argv, 1, mx.rows, &w)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    s = (dyn_scaler_t *)dyn_res_native(ctx, this_val, class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    if (!s) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    /* MinMaxScaler refuses: its parameters are the column MIN and MAX, which
     * are order statistics -- no positive weight can move one, so a weighted
     * fit would accept the option and compute the same thing. That is the
     * silently-ignored-option defect, so it throws instead. */
    minmax = s->minmax;
    if (minmax && w) {
        dyn_matrix_free(&mx); free(w);
        return JS_ThrowTypeError(ctx,
            "MinMaxScaler has no weighted fit: min and max are order "
            "statistics and no positive weight changes them. Drop the rows you "
            "meant to exclude, or use StandardScaler, which weights its mean "
            "and variance.");
    }
    if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx); free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(w);
    return JS_DupValue(ctx, this_val);
}

/* transform / inverseTransform / fitTransform, all shape-in shape-out. */
enum { DYN_SCALE_FWD, DYN_SCALE_INV, DYN_SCALE_FIT_FWD };

static JSValue dyn_scaler_apply(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv,
                                JSClassID class_id, int mode)
{
    dyn_scaler_t *s;
    dyn_matrix_t mx = {0};
    double *out = NULL;
    size_t i, j, rows, cols;
    int flat;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    s = (dyn_scaler_t *)dyn_res_native(ctx, this_val, class_id);
    if (!s) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (mode == DYN_SCALE_FIT_FWD) {
        if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, NULL)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else if (!s->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "transform before fit");
    } else if (mx.cols != s->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, scaler expects %u",
                                 (unsigned)mx.cols, (unsigned)s->n_features);
    }
    rows = mx.rows;
    cols = mx.cols;
    flat = !mx.owned;
    out = (double *)malloc((rows * cols ? rows * cols : 1) * sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * cols;
        double *oi = out + i * cols;
        if (mode == DYN_SCALE_INV)
            for (j = 0; j < cols; j++)
                oi[j] = xi[j] * s->spread[j] + s->centre[j];
        else
            for (j = 0; j < cols; j++)
                oi[j] = (xi[j] - s->centre[j]) * s->scale[j];
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = dyn_ml_matrix_to_js(ctx, out, rows, cols, flat);
    free(out);
    return result;
}

#define DYN_SCALER_METHODS(prefix, class_id_var)                               \
static JSValue prefix##_fit(JSContext *ctx, JSValueConst t, int argc,          \
                            JSValueConst *argv)                                \
{ return dyn_scaler_fit_impl(ctx, t, argc, argv, class_id_var); }              \
static JSValue prefix##_transform(JSContext *ctx, JSValueConst t, int argc,    \
                                  JSValueConst *argv)                          \
{ return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FWD); }  \
static JSValue prefix##_inverse(JSContext *ctx, JSValueConst t, int argc,      \
                                JSValueConst *argv)                            \
{ return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_INV); }  \
static JSValue prefix##_fit_transform(JSContext *ctx, JSValueConst t,          \
                                      int argc, JSValueConst *argv)            \
{ return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FIT_FWD); }

DYN_SCALER_METHODS(dyn_stdscaler, dyn_stdscaler_class_id)
DYN_SCALER_METHODS(dyn_minmax, dyn_minmax_class_id)

/* Getters: the learned statistics, as fresh Arrays (empty before fit). */
static JSValue dyn_scaler_stat(JSContext *ctx, JSValueConst this_val,
                               JSClassID class_id, int which)
{
    dyn_scaler_t *s = (dyn_scaler_t *)dyn_res_native(ctx, this_val, class_id);
    const double *v;
    if (!s)
        return JS_EXCEPTION;
    if (!s->fitted)
        return JS_NewArray(ctx);
    v = which == 0 ? s->centre : s->spread;
    return dyn_ml_doubles_to_js(ctx, v, s->n_features);
}

static JSValue dyn_stdscaler_mean(JSContext *ctx, JSValueConst t)
{ return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 0); }
static JSValue dyn_stdscaler_std(JSContext *ctx, JSValueConst t)
{ return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 1); }
static JSValue dyn_minmax_min(JSContext *ctx, JSValueConst t)
{ return dyn_scaler_stat(ctx, t, dyn_minmax_class_id, 0); }

/* dataMax = min + range, reconstructed so only two vectors are stored. */
static JSValue dyn_minmax_max(JSContext *ctx, JSValueConst this_val)
{
    dyn_scaler_t *s =
        (dyn_scaler_t *)dyn_res_native(ctx, this_val, dyn_minmax_class_id);
    double *tmp;
    size_t j;
    JSValue out;

    if (!s)
        return JS_EXCEPTION;
    if (!s->fitted)
        return JS_NewArray(ctx);
    tmp = (double *)malloc(s->n_features * sizeof(double));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    for (j = 0; j < s->n_features; j++)
        tmp[j] = s->centre[j] + s->spread[j];
    out = dyn_ml_doubles_to_js(ctx, tmp, s->n_features);
    free(tmp);
    return out;
}

static const JSCFunctionListEntry dyn_stdscaler_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_stdscaler_fit),
    JS_CFUNC_DEF("transform", 1, dyn_stdscaler_transform),
    JS_CFUNC_DEF("fitTransform", 1, dyn_stdscaler_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_stdscaler_inverse),
    JS_CGETSET_DEF("mean", dyn_stdscaler_mean, NULL),
    JS_CGETSET_DEF("std", dyn_stdscaler_std, NULL),
};

static const JSCFunctionListEntry dyn_minmax_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_minmax_fit),
    JS_CFUNC_DEF("transform", 1, dyn_minmax_transform),
    JS_CFUNC_DEF("fitTransform", 1, dyn_minmax_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_minmax_inverse),
    JS_CGETSET_DEF("dataMin", dyn_minmax_min, NULL),
    JS_CGETSET_DEF("dataMax", dyn_minmax_max, NULL),
};

/* ---------- SVC: kernel support vector classifier, trained by SMO ------------
 *
 * Sequential Minimal Optimization: the dual QP is solved by repeatedly picking a
 * pair of multipliers and optimising exactly that pair in closed form, which is
 * the only subproblem size for which an analytic solution exists under the
 * equality constraint sum(alpha_i y_i) = 0. No general QP solver, no matrix
 * factorisation.
 *
 * The kernel goes through the same vectorised primitives as everything else:
 * linear is dyn_ml_dot, RBF is exp(-gamma * dyn_ml_sqdist), polynomial is a power
 * of a dot. So the inner loop of training IS the SIMD path.
 *
 * DECISION CACHE. A textbook SMO recomputes f(x_i) = sum_k alpha_k y_k K(x_k, x_i)
 * + b whenever it needs an error, making one sweep O(n^2 * d) and training
 * O(iter * n^2 * d). Instead f is cached for every training point and updated
 * incrementally after each accepted pair step: only two multipliers moved, so the
 * update is two kernel ROWS, O(n * d) per step rather than per sweep. That is the
 * difference between usable and not on a few thousand points.
 *
 * The kernel matrix itself is deliberately NOT cached: it is O(n^2) doubles, which
 * for 10k points is 800 MB. Recomputing a row costs O(n * d) of vectorised work,
 * and this module's whole memory contract is that nothing scales worse than the
 * data.
 *
 * MULTI-CLASS is one-vs-rest: k binary machines, each trained to separate one
 * class from all others, and prediction takes the largest decision value. A
 * classifier that only handled two classes would not be worth shipping. */

typedef enum { DYN_SVM_LINEAR, DYN_SVM_RBF, DYN_SVM_POLY } dyn_svm_kernel_t;

typedef struct {
    double *alpha;   /* n_sv multipliers, already multiplied by their label */
    double *sv;      /* n_sv * cols support vectors */
    double b;
    size_t n_sv;
} dyn_svm_bin_t;

typedef struct {
    int fitted;
    dyn_svm_kernel_t kernel;
    double gamma;    /* 0 => 1/cols, resolved at fit */
    double coef0;
    int degree;
    double C;
    double tol;
    size_t max_iter;
    size_t cols;
    size_t n_classes;
    double *classes;
    dyn_svm_bin_t *bin;   /* n_classes machines, or 1 when n_classes == 2 */
    size_t n_bin;
} dyn_svm_t;

static JSClassID dyn_svm_class_id;

static void dyn_svm_dispose(void *native)
{
    dyn_svm_t *m = (dyn_svm_t *)native;
    size_t i;
    if (!m)
        return;
    if (m->bin) {
        for (i = 0; i < m->n_bin; i++) {
            free(m->bin[i].alpha);
            free(m->bin[i].sv);
        }
        free(m->bin);
    }
    free(m->classes);
    free(m);
}

static const JSClassDef dyn_svm_class = {
    "SVC",
    .finalizer = dyn_res_finalizer,
};

/* new SVC({ kernel, C, gamma, coef0, degree, tol, maxIter }) */
static JSValue dyn_svm_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_svm_t *m;
    dyn_svm_kernel_t kern = DYN_SVM_RBF;
    double C = 1.0, gamma = 0.0, coef0 = 0.0, tol = 1e-3;
    size_t max_iter = 1000, degree_holder = 3;

    (void)new_target;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        JSValue kv = JS_GetPropertyStr(ctx, o, "kernel");
        if (JS_IsException(kv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(kv)) {
            const char *k = JS_ToCString(ctx, kv);
            JS_FreeValue(ctx, kv);
            if (!k)
                return JS_EXCEPTION;
            if (!strcmp(k, "linear")) kern = DYN_SVM_LINEAR;
            else if (!strcmp(k, "rbf")) kern = DYN_SVM_RBF;
            else if (!strcmp(k, "poly")) kern = DYN_SVM_POLY;
            else {
                JS_FreeCString(ctx, k);
                return JS_ThrowTypeError(ctx,
                    "kernel must be \"linear\", \"rbf\" or \"poly\"");
            }
            JS_FreeCString(ctx, k);
        } else {
            JS_FreeValue(ctx, kv);
        }
        if (dyn_opt_double(ctx, o, "C", &C, 1e-12, 1e12) ||
            dyn_opt_double(ctx, o, "gamma", &gamma, 0.0, 1e12) ||
            dyn_opt_double(ctx, o, "coef0", &coef0, -1e12, 1e12) ||
            dyn_opt_double(ctx, o, "tol", &tol, 1e-15, 1.0) ||
            dyn_opt_size(ctx, o, "degree", &degree_holder, 1) ||
            dyn_opt_size(ctx, o, "maxIter", &max_iter, 1))
            return JS_EXCEPTION;
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    /* degree is an int below; past 1000 it is only ever a mistake or a
     * poly-kernel CPU bomb (O(degree) per kernel eval per support vector) */
    if (degree_holder > 1000)
        return JS_ThrowRangeError(ctx, "degree must be at most 1000");
    /* SMO is O(maxIter * rows) kernel evals per binary machine */
    if (max_iter > DYN_ML_MAX_ITERS)
        return JS_ThrowRangeError(ctx, "maxIter must be at most %u",
                                  (unsigned)DYN_ML_MAX_ITERS);
    m = (dyn_svm_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->kernel = kern;
    m->C = C;
    m->gamma = gamma;
    m->coef0 = coef0;
    m->degree = (int)degree_holder;
    m->tol = tol;
    m->max_iter = max_iter;
    return dyn_res_wrap(ctx, dyn_svm_class_id, m, dyn_svm_dispose);
}

static double dyn_svm_k(const dyn_svm_t *m, const double *a, const double *b,
                        size_t d, double gamma)
{
    switch (m->kernel) {
    case DYN_SVM_LINEAR:
        return dyn_ml_dot(a, b, d);
    case DYN_SVM_POLY: {
        double base = gamma * dyn_ml_dot(a, b, d) + m->coef0;
        double r = 1.0;
        int e;
        for (e = 0; e < m->degree; e++)
            r *= base;   /* integer power; overflows to +-inf for large base --
                          * the CALLERS check isfinite and refuse, because an
                          * inf kernel reaches NaN alphas through eta, and NaN
                          * alphas slip past this file's copy filter into a
                          * buffer sized by the finite-only count (heap OOB) */
        return r;
    }
    default:
        return exp(-gamma * dyn_ml_sqdist(a, b, d));
    }
}

/* Train one binary machine on labels yb[i] in {+1, -1}. Fills *out with the
 * support vectors and their signed multipliers. Returns 0, or -1 (throwing). */
static int dyn_svm_train_bin(JSContext *ctx, dyn_svm_t *m, const double *X,
                             const double *yb, size_t rows, size_t cols,
                             double gamma, dyn_svm_bin_t *out, uint64_t *rng)
{
    double *alpha = NULL, *f = NULL, *krow_i = NULL, *krow_j = NULL;
    double b = 0.0;
    size_t iter, i, j, nsv, s;
    int rc = -1;

    alpha = (double *)calloc(rows, sizeof(double));
    f = (double *)calloc(rows, sizeof(double));   /* f == b == 0 at alpha == 0 */
    krow_i = (double *)malloc(rows * sizeof(double));
    krow_j = (double *)malloc(rows * sizeof(double));
    if (!alpha || !f || !krow_i || !krow_j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (iter = 0; iter < m->max_iter; iter++) {
        size_t changed = 0;
        for (i = 0; i < rows; i++) {
            double Ei = f[i] - yb[i];
            double ri = yb[i] * Ei;
            /* KKT violation on i: either below C and misclassified, or above 0
             * and over-satisfied */
            if (!((ri < -m->tol && alpha[i] < m->C) ||
                  (ri > m->tol && alpha[i] > 0.0)))
                continue;
            j = (size_t)(dyn_splitmix64(rng) % (uint64_t)rows);
            if (j == i)
                j = (i + 1) % rows;
            {
                double Ej = f[j] - yb[j];
                double ai_old = alpha[i], aj_old = alpha[j];
                double L, H, eta, kii, kjj, kij, aj_new, ai_new, b1, b2, b_new;
                double di, dj;

                if (yb[i] != yb[j]) {
                    L = fmax(0.0, aj_old - ai_old);
                    H = fmin(m->C, m->C + aj_old - ai_old);
                } else {
                    L = fmax(0.0, ai_old + aj_old - m->C);
                    H = fmin(m->C, ai_old + aj_old);
                }
                if (H - L < 1e-15)
                    continue;
                kii = dyn_svm_k(m, X + i * cols, X + i * cols, cols, gamma);
                kjj = dyn_svm_k(m, X + j * cols, X + j * cols, cols, gamma);
                kij = dyn_svm_k(m, X + i * cols, X + j * cols, cols, gamma);
                if (!isfinite(kii) || !isfinite(kjj) || !isfinite(kij)) {
                    JS_ThrowRangeError(ctx,
                        "polynomial kernel overflowed; scale X or lower degree");
                    goto done;
                }
                eta = kii + kjj - 2.0 * kij;
                /* NaN fails the <= comparison too: an inf eta from huge finite
                 * kernels would flow into aj_new and poison alpha with NaN */
                if (!isfinite(eta) || eta <= 1e-15)
                    continue;      /* not positive definite in this direction */
                aj_new = aj_old + yb[j] * (Ei - Ej) / eta;
                if (aj_new > H) aj_new = H;
                if (aj_new < L) aj_new = L;
                if (fabs(aj_new - aj_old) < 1e-12)
                    continue;
                ai_new = ai_old + yb[i] * yb[j] * (aj_old - aj_new);
                di = yb[i] * (ai_new - ai_old);
                dj = yb[j] * (aj_new - aj_old);
                b1 = b - Ei - di * kii - dj * kij;
                b2 = b - Ej - di * kij - dj * kjj;
                if (ai_new > 0.0 && ai_new < m->C)
                    b_new = b1;
                else if (aj_new > 0.0 && aj_new < m->C)
                    b_new = b2;
                else
                    b_new = 0.5 * (b1 + b2);
                /* incremental refresh of the decision cache: only two
                 * multipliers and the bias moved, so two kernel rows suffice */
                for (s = 0; s < rows; s++) {
                    krow_i[s] = dyn_svm_k(m, X + i * cols, X + s * cols, cols, gamma);
                    krow_j[s] = dyn_svm_k(m, X + j * cols, X + s * cols, cols, gamma);
                    if (!isfinite(krow_i[s]) || !isfinite(krow_j[s])) {
                        JS_ThrowRangeError(ctx,
                            "polynomial kernel overflowed; scale X or lower degree");
                        goto done;
                    }
                }
                for (s = 0; s < rows; s++)
                    f[s] += di * krow_i[s] + dj * krow_j[s] + (b_new - b);
                alpha[i] = ai_new;
                alpha[j] = aj_new;
                b = b_new;
                changed++;
            }
        }
        if (changed == 0)
            break;                 /* every KKT condition satisfied to tol */
    }
    /* keep only the support vectors (alpha > 0): everything else contributes
     * nothing to a prediction and would just be memory */
    nsv = 0;
    for (i = 0; i < rows; i++)
        if (alpha[i] > 1e-12)
            nsv++;
    out->alpha = (double *)malloc((nsv ? nsv : 1) * sizeof(double));
    out->sv = (double *)malloc((nsv ? nsv * cols : 1) * sizeof(double));
    if (!out->alpha || !out->sv) {
        free(out->alpha); free(out->sv);
        out->alpha = NULL; out->sv = NULL;
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    nsv = 0;
    for (i = 0; i < rows; i++) {
        /* `!(> 1e-12)`, not `<= 1e-12`: a NaN alpha fails BOTH comparisons,
         * and must be skipped by this loop exactly as the count above skipped
         * it -- counting one way and copying the other wrote past the buffer */
        if (!(alpha[i] > 1e-12))
            continue;
        out->alpha[nsv] = alpha[i] * yb[i];   /* fold the label in once */
        memcpy(out->sv + nsv * cols, X + i * cols, cols * sizeof(double));
        nsv++;
    }
    out->n_sv = nsv;
    out->b = b;
    rc = 0;
done:
    free(alpha); free(f); free(krow_i); free(krow_j);
    return rc;
}

static double dyn_svm_decide(const dyn_svm_t *m, const dyn_svm_bin_t *bin,
                             const double *x, double gamma)
{
    double s = bin->b;
    size_t i;
    for (i = 0; i < bin->n_sv; i++)
        s += bin->alpha[i] *
             dyn_svm_k(m, bin->sv + i * m->cols, x, m->cols, gamma);
    return s;
}

static int dyn_svm_learn(JSContext *ctx, dyn_svm_t *m, const double *X,
                         const double *y, size_t rows, size_t cols)
{
    double *classes = NULL, *yb = NULL;
    dyn_svm_bin_t *bin = NULL;
    size_t nc, nb, k, i;
    double gamma;
    uint64_t rng = 987654321ULL;
    int rc = -1;

    nc = dyn_ml_classes(ctx, y, rows, &classes);
    if (nc == 0)
        return -1;
    if (nc < 2) {
        free(classes);
        JS_ThrowRangeError(ctx, "SVC needs at least two classes");
        return -1;
    }
    /* one-vs-rest trains nb machines, each O(maxIter * rows) kernel evals --
     * the one class-count axis with no cap. 256 matches the tree/logreg cap
     * and is far past any real one-vs-rest use. */
    if (nc > 256) {
        free(classes);
        JS_ThrowRangeError(ctx, "SVC supports at most 256 classes, found %u",
                           (unsigned)nc);
        return -1;
    }
    gamma = (m->gamma > 0.0) ? m->gamma : 1.0 / (double)cols;
    nb = (nc == 2) ? 1 : nc;
    bin = (dyn_svm_bin_t *)calloc(nb, sizeof(*bin));
    yb = (double *)malloc(rows * sizeof(double));
    if (!bin || !yb) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    m->cols = cols;
    for (k = 0; k < nb; k++) {
        /* two classes: one machine, +1 for the SECOND class so the sign of the
         * decision value maps to classes[1]. more: one-vs-rest per class. */
        double pos = (nc == 2) ? classes[1] : classes[k];
        for (i = 0; i < rows; i++)
            yb[i] = (y[i] == pos) ? 1.0 : -1.0;
        if (dyn_svm_train_bin(ctx, m, X, yb, rows, cols, gamma, &bin[k], &rng))
            goto done;
    }
    if (m->bin) {
        for (k = 0; k < m->n_bin; k++) {
            free(m->bin[k].alpha);
            free(m->bin[k].sv);
        }
        free(m->bin);
    }
    free(m->classes);
    m->bin = bin;
    m->n_bin = nb;
    m->classes = classes;
    m->n_classes = nc;
    m->gamma = gamma;
    m->fitted = 1;
    bin = NULL;
    classes = NULL;
    rc = 0;
done:
    if (bin) {
        for (k = 0; k < nb; k++) {
            free(bin[k].alpha);
            free(bin[k].sv);
        }
        free(bin);
    }
    free(classes);
    free(yb);
    return rc;
}

static JSValue dyn_svm_fit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). */
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "SVC"))
        return JS_EXCEPTION;
    dyn_svm_t *m;
    dyn_matrix_t mx = {0};
    double *y = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg, &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_svm_t *)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    if (dyn_svm_learn(ctx, m, mx.data, y, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx); free(y);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

/* predict -> labels; decisionFunction -> the raw margin (binary) or the
 * per-class one-vs-rest values (multi-class). */
static JSValue dyn_svm_predict_impl(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int decision)
{
    dyn_svm_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL;
    size_t rows, i, k, ocols;
    int flat;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    /* a NaN row cannot win an argmax, so it would silently take classes[0] */
    if (dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_svm_t *)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->cols);
    }
    rows = mx.rows;
    flat = !mx.owned;
    ocols = decision ? m->n_bin : 1;
    if (rows > (SIZE_MAX / sizeof(double)) / ocols) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "data too large for SVC predict");
    }
    out = (double *)malloc(rows * ocols * sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double *xi = mx.data + i * mx.cols;
        /* a poly-kernel decision can overflow to inf/NaN; refusing beats a
         * silently garbage class (NaN loses every argmax) */
#define DYN_SVM_REFUSE_NONFINITE(dv)                                           \
        do { if (!isfinite(dv)) {                                              \
            free(out); dyn_matrix_free(&mx);                                   \
            return JS_ThrowRangeError(ctx,                                     \
                "polynomial kernel overflowed; scale X or lower degree");      \
        } } while (0)
        if (decision) {
            for (k = 0; k < m->n_bin; k++) {
                out[i * ocols + k] = dyn_svm_decide(m, &m->bin[k], xi, m->gamma);
                DYN_SVM_REFUSE_NONFINITE(out[i * ocols + k]);
            }
        } else if (m->n_bin == 1) {
            double d0 = dyn_svm_decide(m, &m->bin[0], xi, m->gamma);
            DYN_SVM_REFUSE_NONFINITE(d0);
            out[i] = (d0 >= 0.0) ? m->classes[1] : m->classes[0];
        } else {
            double best = -HUGE_VAL;
            size_t bestk = 0;
            for (k = 0; k < m->n_bin; k++) {
                double d = dyn_svm_decide(m, &m->bin[k], xi, m->gamma);
                DYN_SVM_REFUSE_NONFINITE(d);
                if (d > best) {
                    best = d;
                    bestk = k;
                }
            }
            out[i] = m->classes[bestk];
        }
    }
#undef DYN_SVM_REFUSE_NONFINITE
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = (decision && ocols > 1)
                 ? dyn_ml_matrix_to_js(ctx, out, rows, ocols, flat)
                 : dyn_ml_doubles_to_js(ctx, out, rows);
    free(out);
    return result;
}

static JSValue dyn_svm_predict(JSContext *ctx, JSValueConst t, int argc,
                               JSValueConst *argv)
{ return dyn_svm_predict_impl(ctx, t, argc, argv, 0); }
static JSValue dyn_svm_decision(JSContext *ctx, JSValueConst t, int argc,
                                JSValueConst *argv)
{ return dyn_svm_predict_impl(ctx, t, argc, argv, 1); }

static JSValue dyn_svm_nsv(JSContext *ctx, JSValueConst this_val)
{
    dyn_svm_t *m = (dyn_svm_t *)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    size_t k, tot = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    for (k = 0; k < m->n_bin; k++)
        tot += m->bin[k].n_sv;
    return JS_NewInt64(ctx, (int64_t)tot);
}

static JSValue dyn_svm_classes_get(JSContext *ctx, JSValueConst this_val)
{
    dyn_svm_t *m = (dyn_svm_t *)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

static const JSCFunctionListEntry dyn_svm_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_svm_fit),
    JS_CFUNC_DEF("predict", 1, dyn_svm_predict),
    JS_CFUNC_DEF("decisionFunction", 1, dyn_svm_decision),
    JS_CGETSET_DEF("nSupportVectors", dyn_svm_nsv, NULL),
    JS_CGETSET_DEF("classes", dyn_svm_classes_get, NULL),
};

/* ---------- GaussianMixture: EM with diagonal covariances -------------------
 *
 * Soft clustering: each component is a Gaussian with its own mean, per-feature
 * variance and mixing weight, fitted by Expectation-Maximisation.
 *
 * Diagonal covariances, not full. A full covariance per component is cols x cols
 * to store, needs a Cholesky factorisation per component per iteration, and needs
 * more samples than features to be non-singular at all. Diagonal keeps the whole
 * fit O(iter * rows * k * cols) with no factorisation and stays defined for wide
 * data -- the standard trade for a general-purpose implementation.
 *
 * Everything is in LOG space for the same reason as GaussianNB: the density is a
 * product over features and underflows quickly. Responsibilities are normalised
 * with log-sum-exp about the row maximum, which also yields the per-row
 * log-likelihood contribution for free, so convergence is tested on the true
 * total log-likelihood rather than a proxy.
 *
 * Initialisation reuses the k-means++ seeding already in this file, then one hard
 * assignment, which gives EM a far better starting point than random means and
 * makes the result reproducible from the seed. reg_covar is added to every
 * variance so a component that collapses onto a single point cannot produce an
 * infinite density -- the classic way an EM implementation blows up. */

typedef struct {
    int fitted;
    size_t k;
    size_t cols;
    size_t max_iter;
    double tol;
    double reg;
    uint64_t seed;
    double *weight;     /* k mixing weights */
    double *mean;       /* k * cols */
    double *var;        /* k * cols (regularised) */
    /* Derived after every M step so the E step's inner loop holds no call and no
     * division -- see dyn_gmm_logpdf. Recomputing these is O(k*cols) against the
     * E step's O(rows*k*cols), i.e. free. */
    double *inv_var;    /* k * cols: 1/var */
    double *lognorm;    /* k: log(weight) - 0.5*sum_j log(2*pi*var[j]) */
    double loglik;
    size_t n_iter;
} dyn_gmm_t;

static JSClassID dyn_gmm_class_id;

static void dyn_gmm_dispose(void *native)
{
    dyn_gmm_t *m = (dyn_gmm_t *)native;
    if (m) {
        free(m->weight);
        free(m->mean);
        free(m->var);
        free(m->inv_var);
        free(m->lognorm);
        free(m);
    }
}

static const JSClassDef dyn_gmm_class = {
    "GaussianMixture",
    .finalizer = dyn_res_finalizer,
};

/* new GaussianMixture(k = 3, { seed, maxIter, tol, regCovar }) */
static JSValue dyn_gmm_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv)
{
    dyn_gmm_t *m;
    int64_t k = 3;
    size_t max_iter = 200, seed_holder = 12345;
    double tol = 1e-3, reg = 1e-6;

    (void)new_target;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k < 1)
            return JS_ThrowRangeError(ctx, "nComponents must be at least 1");
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opt_size(ctx, argv[1], "maxIter", &max_iter, 1) ||
            dyn_opt_size(ctx, argv[1], "seed", &seed_holder, 0) ||
            dyn_opt_double(ctx, argv[1], "tol", &tol, 0.0, 1e12) ||
            dyn_opt_double(ctx, argv[1], "regCovar", &reg, 0.0, 1e12))
            return JS_EXCEPTION;
    } else if (argc > 1 && !JS_IsUndefined(argv[1])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    /* EM runs maxIter * O(rows*k*cols); dyn_opt_size has no upper bound, so a
     * hostile maxIter stalls the engine on the main thread */
    if (max_iter > DYN_ML_MAX_EM_ITERS)
        return JS_ThrowRangeError(ctx, "maxIter must be at most %u",
                                  (unsigned)DYN_ML_MAX_EM_ITERS);
    m = (dyn_gmm_t *)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = (size_t)k;
    m->max_iter = max_iter;
    m->tol = tol;
    m->reg = reg;
    m->seed = (uint64_t)seed_holder;
    return dyn_res_wrap(ctx, dyn_gmm_class_id, m, dyn_gmm_dispose);
}

/* log N(x | mean_c, diag(var_c)) + log weight_c.
 *
 * Everything constant in x -- log(weight[c]) and the sum of log(2*pi*var[j]) --
 * lives in lognorm[c], and the division by var[j] is a multiply by a precomputed
 * reciprocal. What is left is an arithmetic reduction over two contiguous arrays
 * with four independent accumulators, which VECTORISES (verified in the assembly:
 * fmla.2d, zero `bl _log`). Previously this made `cols` libm calls for every
 * (row, component) of every EM iteration -- the single largest cost in the fit,
 * and unvectorisable by construction because of the call. */
static double dyn_gmm_logpdf(const dyn_gmm_t *m, const double *x, size_t c)
{
    const double *mu = m->mean + c * m->cols;
    const double *iv = m->inv_var + c * m->cols;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, d;
    size_t n = m->cols, j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        d = x[j] - mu[j];         s0 += d * d * iv[j];
        d = x[j + 1] - mu[j + 1]; s1 += d * d * iv[j + 1];
        d = x[j + 2] - mu[j + 2]; s2 += d * d * iv[j + 2];
        d = x[j + 3] - mu[j + 3]; s3 += d * d * iv[j + 3];
    }
#endif
    for (; j < n; j++) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
    }
    return m->lognorm[c] - 0.5 * ((s0 + s1) + (s2 + s3));
}

/* Refresh inv_var/lognorm from weight/var. Must run after every change to either
 * -- the initial statistics and each M step. */
static void dyn_gmm_refresh(dyn_gmm_t *m)
{
    size_t c, j;
    for (c = 0; c < m->k; c++) {
        double ld = 0.0;
        for (j = 0; j < m->cols; j++) {
            double v = m->var[c * m->cols + j];
            /* regCovar may be 0 (and a dead component's variance is NaN), so
             * clamp before the reciprocal: 1/0 and log(0) would poison the
             * whole E step and the convergence test with them */
            if (!(v > 0.0))
                v = 1e-12;
            m->inv_var[c * m->cols + j] = 1.0 / v;
            ld += log(6.283185307179586476925286766559 * v);
        }
        m->lognorm[c] = log(m->weight[c]) - 0.5 * ld;
    }
}

/* One E step: responsibilities into resp[rows*k], returning the total log
 * likelihood. */
static double dyn_gmm_estep(const dyn_gmm_t *m, const double *X, size_t rows,
                            double *resp)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double *xi = X + i * m->cols;
        double best = -HUGE_VAL, sum = 0.0;
        for (c = 0; c < m->k; c++) {
            resp[i * m->k + c] = dyn_gmm_logpdf(m, xi, c);
            if (resp[i * m->k + c] > best)
                best = resp[i * m->k + c];
        }
        for (c = 0; c < m->k; c++) {
            resp[i * m->k + c] = exp(resp[i * m->k + c] - best);
            sum += resp[i * m->k + c];
        }
        for (c = 0; c < m->k; c++)
            resp[i * m->k + c] /= sum;
        total += best + log(sum);   /* log-sum-exp: the exact row likelihood */
    }
    return total;
}

static int dyn_gmm_learn(JSContext *ctx, dyn_gmm_t *m, const double *X,
                         size_t rows, size_t cols)
{
    double *weight = NULL, *mean = NULL, *var = NULL, *resp = NULL;
    double *nk_all = NULL;   /* per-component responsibility sums, one pass */
    double *inv_var = NULL, *lognorm = NULL, *nearest = NULL;
    int *labels = NULL;
    size_t k = m->k, i, c, j, it;
    uint64_t rng = m->seed;
    double prev = -HUGE_VAL, ll = -HUGE_VAL;
    int rc = -1;

    if (rows < k) {
        JS_ThrowRangeError(ctx,
            "GaussianMixture needs at least nComponents rows");
        return -1;
    }
    /* resp is rows*k doubles; rows ~ k ~ 2^31 wraps the product, and the
     * wrapped malloc is a small VALID buffer the E step then overflows */
    if (rows > (SIZE_MAX / sizeof(double)) / k) {
        JS_ThrowRangeError(ctx, "data too large for GaussianMixture");
        return -1;
    }
    weight = (double *)malloc(k * sizeof(double));
    mean = (double *)malloc(k * cols * sizeof(double));
    var = (double *)malloc(k * cols * sizeof(double));
    inv_var = (double *)malloc(k * cols * sizeof(double));
    lognorm = (double *)malloc(k * sizeof(double));
    resp = (double *)malloc(rows * k * sizeof(double));
    nk_all = (double *)malloc((k ? k : 1) * sizeof(double));
    nearest = (double *)malloc(rows * sizeof(double));
    labels = (int *)malloc(rows * sizeof(int));
    if (!weight || !mean || !var || !inv_var || !lognorm || !resp ||
        !nearest || !labels || !nk_all) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    /* k-means++ seeding, then one hard assignment to get initial statistics */
    dyn_km_plusplus(X, rows, cols, k, mean, nearest, &rng);
    dyn_km_assign(X, rows, cols, mean, k, labels, NULL);
    m->cols = cols;
    m->weight = weight;
    m->mean = mean;
    m->var = var;
    m->inv_var = inv_var;
    m->lognorm = lognorm;
    for (c = 0; c < k; c++) {
        weight[c] = 0.0;
        for (j = 0; j < cols; j++)
            var[c * cols + j] = 0.0;
    }
    for (i = 0; i < rows; i++) {
        size_t lb = (size_t)labels[i];
        weight[lb] += 1.0;
        for (j = 0; j < cols; j++) {
            double d = X[i * cols + j] - mean[lb * cols + j];
            var[lb * cols + j] += d * d;
        }
    }
    for (c = 0; c < k; c++) {
        double cnt = weight[c];
        for (j = 0; j < cols; j++)
            var[c * cols + j] = (cnt > 0.0 ? var[c * cols + j] / cnt : 1.0) + m->reg;
        /* an empty component keeps a positive weight so its log is finite; EM
         * will either revive it or leave it negligible */
        weight[c] = (cnt > 0.0 ? cnt : 1.0) / (double)rows;
    }
    dyn_gmm_refresh(m);
    for (it = 0; it < m->max_iter; it++) {
        ll = dyn_gmm_estep(m, X, rows, resp);
        /* M step.
         *
         * Component-OUTER streamed X twice per component -- 2k passes over the
         * whole matrix per EM iteration, against the E step's one -- and read
         * resp with stride k, using one double per cache line, k times over.
         * Row-outer instead: one pass for the responsibility sums, one to
         * accumulate every component's mean, one for every variance. 3 sweeps
         * instead of 3k, and resp is read sequentially.
         *
         * BIT-IDENTICAL: for a fixed (c, j) the summation over i still runs in
         * ascending i, so no floating-point sum is reassociated. The scratch is
         * k * cols doubles.
         */
        for (c = 0; c < k; c++)
            nk_all[c] = 0.0;
        for (i = 0; i < rows; i++)
            for (c = 0; c < k; c++)
                nk_all[c] += resp[i * k + c];

        for (c = 0; c < k; c++)
            for (j = 0; j < cols; j++)
                mean[c * cols + j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            for (c = 0; c < k; c++) {
                double r = resp[i * k + c];
                if (nk_all[c] > 1e-300)
                    dyn_ml_axpy(mean + c * cols, r, xi, cols);
            }
        }
        for (c = 0; c < k; c++) {
            if (nk_all[c] <= 1e-300)
                continue;
            for (j = 0; j < cols; j++)
                mean[c * cols + j] /= nk_all[c];
        }

        for (c = 0; c < k; c++)
            for (j = 0; j < cols; j++)
                var[c * cols + j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double *xi = X + i * cols;
            for (c = 0; c < k; c++) {
                double r = resp[i * k + c];
                if (nk_all[c] <= 1e-300)
                    continue;
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - mean[c * cols + j];
                    var[c * cols + j] += r * d * d;
                }
            }
        }
        for (c = 0; c < k; c++) {
            if (nk_all[c] <= 1e-300) {
                weight[c] = 1e-300;   /* collapsed: leave parameters alone */
                continue;
            }
            for (j = 0; j < cols; j++)
                var[c * cols + j] = var[c * cols + j] / nk_all[c] + m->reg;
            weight[c] = nk_all[c] / (double)rows;
        }
        dyn_gmm_refresh(m);        /* parameters changed: derived values follow */
        m->n_iter = it + 1;
        if (it > 0 && fabs(ll - prev) < m->tol * fabs(ll ? ll : 1.0))
            break;                 /* relative log-likelihood change below tol */
        prev = ll;
    }
    m->loglik = ll;
    m->fitted = 1;
    weight = NULL; mean = NULL; var = NULL;   /* now owned by the model */
    inv_var = NULL; lognorm = NULL;
    rc = 0;
done:
    if (rc != 0) {
        free(weight); free(mean); free(var); free(inv_var); free(lognorm);
        m->weight = NULL; m->mean = NULL; m->var = NULL;
        m->inv_var = NULL; m->lognorm = NULL;
    }
    free(resp); free(nearest); free(labels); free(nk_all);
    return rc;
}

static JSValue dyn_gmm_fit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    /* No weighted form: refuse rather than silently ignore (W9.4). */
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "GaussianMixture"))
        return JS_EXCEPTION;
    dyn_gmm_t *m;
    dyn_matrix_t mx = {0};
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    /* dyn_gmm_learn installs its new arrays into m before it finishes, so the
     * previous ones must be kept until it has succeeded: a failed refit leaves
     * the old model usable, and a successful one frees the old arrays after. */
    {
        double *ow = m->weight, *om = m->mean, *ov = m->var;
        double *oi = m->inv_var, *ol = m->lognorm;
        if (dyn_gmm_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
        free(ow); free(om); free(ov); free(oi); free(ol);
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_gmm_predict_impl(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int proba)
{
    dyn_gmm_t *m;
    dyn_matrix_t mx = {0};
    double *out = NULL;
    size_t rows, i, c;
    int flat;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    /* a NaN row's log-density loses every argmax and lands in component 0 */
    if (dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "predict before fit");
    }
    if (mx.cols != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "X has %u features, model expects %u",
                                 (unsigned)mx.cols, (unsigned)m->cols);
    }
    rows = mx.rows;
    flat = !mx.owned;
    /* rows*k doubles can wrap when the loaded model's k is huge */
    if (proba && (m->k == 0 || rows > (SIZE_MAX / sizeof(double)) / m->k)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "data too large for predictProba");
    }
    out = (double *)malloc(rows * (proba ? m->k : 1) * sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (proba) {
        dyn_gmm_estep(m, mx.data, rows, out);
    } else {
        double *sc = (double *)malloc(m->k * sizeof(double));
        if (!sc) {
            free(out);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < rows; i++) {
            const double *xi = mx.data + i * mx.cols;
            double best = -HUGE_VAL;
            size_t bestc = 0;
            for (c = 0; c < m->k; c++) {
                sc[c] = dyn_gmm_logpdf(m, xi, c);
                if (sc[c] > best) {
                    best = sc[c];
                    bestc = c;
                }
            }
            out[i] = (double)bestc;
        }
        free(sc);
    }
    dyn_matrix_free(&mx); /* done reading X (incl. any alias) before any JS */
    result = proba ? dyn_ml_matrix_to_js(ctx, out, rows, m->k, flat)
                   : dyn_ml_doubles_to_js(ctx, out, rows);
    free(out);
    return result;
}

static JSValue dyn_gmm_predict(JSContext *ctx, JSValueConst t, int argc,
                               JSValueConst *argv)
{ return dyn_gmm_predict_impl(ctx, t, argc, argv, 0); }
static JSValue dyn_gmm_predict_proba(JSContext *ctx, JSValueConst t, int argc,
                                     JSValueConst *argv)
{ return dyn_gmm_predict_impl(ctx, t, argc, argv, 1); }

static JSValue dyn_gmm_weights(JSContext *ctx, JSValueConst this_val)
{
    dyn_gmm_t *m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->weight, m->k);
}

static JSValue dyn_gmm_means(JSContext *ctx, JSValueConst this_val)
{
    dyn_gmm_t *m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->mean, m->k, m->cols, 0);
}

static JSValue dyn_gmm_variances(JSContext *ctx, JSValueConst this_val)
{
    dyn_gmm_t *m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->var, m->k, m->cols, 0);
}

static JSValue dyn_gmm_loglik(JSContext *ctx, JSValueConst this_val)
{
    dyn_gmm_t *m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->loglik : 0.0);
}

static JSValue dyn_gmm_niter(JSContext *ctx, JSValueConst this_val)
{
    dyn_gmm_t *m = (dyn_gmm_t *)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->n_iter : 0));
}

static const JSCFunctionListEntry dyn_gmm_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_gmm_fit),
    JS_CFUNC_DEF("predict", 1, dyn_gmm_predict),
    JS_CFUNC_DEF("predictProba", 1, dyn_gmm_predict_proba),
    JS_CGETSET_DEF("weights", dyn_gmm_weights, NULL),
    JS_CGETSET_DEF("means", dyn_gmm_means, NULL),
    JS_CGETSET_DEF("variances", dyn_gmm_variances, NULL),
    JS_CGETSET_DEF("logLikelihood", dyn_gmm_loglik, NULL),
    JS_CGETSET_DEF("nIter", dyn_gmm_niter, NULL),
};

/* ---------- metrics: plain transient functions, no resource ------------------
 *
 * Each takes two equal-length vectors (Array or Float64Array) and returns a
 * Number. They are O(n) single-pass, so the kernel dispatch would not amortise
 * except on the one genuinely contiguous reduction (the mean inside r2Score),
 * which uses the multi-accumulator dyn_ml_sum. */

/* Ingest two equal-length vectors. Both are OWNED copies (never aliases), so a
 * hostile valueOf during the second ingest cannot invalidate the first. */
static int dyn_metric_pair(JSContext *ctx, JSValueConst av, JSValueConst bv,
                           double **pa, double **pb, size_t *pn)
{
    size_t n;

    if (JS_IsArray(ctx, av)) {
        if (dyn_ml_len(ctx, av, &n))
            return -1;
    } else {
        double *tmp;
        if (dyn_ml_get_f64(ctx, av, &tmp, &n))
            return -1;
    }
    if (n == 0) {
        JS_ThrowRangeError(ctx, "metric needs at least one sample");
        return -1;
    }
    if (dyn_ml_ingest_vector(ctx, av, n, pa))
        return -1;
    if (dyn_ml_ingest_vector(ctx, bv, n, pb)) {
        free(*pa);
        *pa = NULL;
        return -1;
    }
    *pn = n;
    return 0;
}

enum { DYN_METRIC_MSE, DYN_METRIC_MAE, DYN_METRIC_R2, DYN_METRIC_LOGLOSS,
       DYN_METRIC_ACCURACY };

/* Multiclass log loss. `yPred` is rows x classes with the columns in ascending
 * label order, which is exactly what every classifier's predictProba returns.
 *
 * This exists because it was MISSING and the absence was silent: a matrix
 * argument coerced to NaN per row, so logLoss(y, model.predictProba(X)) --
 * the composition the metric was added for -- answered NaN rather than
 * throwing. A metric that cannot read the shape it is given must say so. */
static JSValue dyn_metric_logloss_multi(JSContext *ctx, JSValueConst yv,
                                        JSValueConst pv)
{
    dyn_matrix_t P = {0};
    double *yt = NULL, *classes = NULL;
    size_t n_classes, i, k;
    double acc = 0.0;
    const double EPS = 1e-15;

    if (dyn_ml_ingest_matrix_array(ctx, pv, &P))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_vector(ctx, yv, P.rows, &yt)) {
        dyn_matrix_free(&P);
        return JS_EXCEPTION;
    }
    /* a NaN label is its own class under !=, inflating n_classes and scoring
     * a different model than the caller has; refuse like stratifiedKFold */
    for (i = 0; i < P.rows; i++)
        if (!isfinite(yt[i])) {
            dyn_matrix_free(&P);
            free(yt);
            return JS_ThrowRangeError(ctx, "logLoss labels must be finite");
        }
    n_classes = dyn_ml_classes(ctx, yt, P.rows, &classes);
    if (n_classes == 0) {
        dyn_matrix_free(&P);
        free(yt);
        return JS_EXCEPTION;
    }
    /* A y that does not name every column cannot say which column is true for
     * a row, and guessing would score a different model than the caller has. */
    if (n_classes != P.cols) {
        dyn_matrix_free(&P);
        free(yt);
        free(classes);
        return JS_ThrowTypeError(ctx,
            "yPred has %u columns but yTrue names %u distinct labels; "
            "the columns are the classes in ascending label order",
            (unsigned)P.cols, (unsigned)n_classes);
    }
    for (i = 0; i < P.rows; i++) {
        double p = 0.0;
        for (k = 0; k < n_classes; k++)
            if (classes[k] == yt[i])
                p = P.data[i * P.cols + k];
        /* NaN fails both comparisons, so it must be caught explicitly; a
         * NaN probability would otherwise poison the whole mean silently */
        if (!(p >= EPS))
            p = EPS;
        else if (!(p <= 1.0))
            p = 1.0;
        acc += -log(p);
    }
    {
        double result = P.rows ? acc / (double)P.rows : 0.0;
        dyn_matrix_free(&P);
        free(yt);
        free(classes);
        return JS_NewFloat64(ctx, result);
    }
}

/* Is `v` a matrix -- an array whose first element is itself an array? Used only
 * to pick logLoss's shape, so a wrong answer is a clear type error rather than
 * a wrong number. */
static int dyn_ml_is_matrix(JSContext *ctx, JSValueConst v)
{
    JSValue first;
    int yes;
    if (!JS_IsArray(ctx, v))
        return 0;
    first = JS_GetPropertyUint32(ctx, v, 0);
    if (JS_IsException(first)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    yes = JS_IsArray(ctx, first);
    JS_FreeValue(ctx, first);
    return yes;
}

static JSValue dyn_metric(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int magic)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i;
    double acc = 0.0, result;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "expected (yTrue, yPred)");
    if (magic == DYN_METRIC_LOGLOSS && dyn_ml_is_matrix(ctx, argv[1]))
        return dyn_metric_logloss_multi(ctx, argv[0], argv[1]);
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    /* a NaN label or prediction would propagate into the mean and answer NaN
     * silently -- the same refusal rocAuc already enforces */
    for (i = 0; i < n; i++)
        if (!isfinite(yt[i]) || !isfinite(yp[i])) {
            free(yt); free(yp);
            return JS_ThrowRangeError(ctx,
                "metric inputs must be finite");
        }

    switch (magic) {
    case DYN_METRIC_MSE:
        for (i = 0; i < n; i++) {
            double d = yt[i] - yp[i];
            acc += d * d;
        }
        result = acc / (double)n;
        break;
    case DYN_METRIC_MAE:
        for (i = 0; i < n; i++)
            acc += fabs(yt[i] - yp[i]);
        result = acc / (double)n;
        break;
    case DYN_METRIC_R2: {
        /* 1 - SS_res/SS_tot. A constant y_true gives SS_tot == 0: report 1.0 if
         * the prediction is exact, else 0.0 -- scikit-learn's convention, and
         * better than NaN or a division by zero. */
        double mean = dyn_ml_sum(yt, n) / (double)n;
        double ss_res = 0.0, ss_tot = 0.0;
        for (i = 0; i < n; i++) {
            double dr = yt[i] - yp[i];
            double dt = yt[i] - mean;
            ss_res += dr * dr;
            ss_tot += dt * dt;
        }
        if (ss_tot == 0.0)
            result = (ss_res == 0.0) ? 1.0 : 0.0;
        else
            result = 1.0 - ss_res / ss_tot;
        break;
    }
    case DYN_METRIC_LOGLOSS: {
        /* Mean negative log-likelihood. Probabilities are clipped away from 0
         * and 1 (as scikit-learn does) so a confident wrong prediction is a
         * large finite penalty instead of an infinity that swallows the mean. */
        const double EPS = 1e-15;
        for (i = 0; i < n; i++) {
            double p = yp[i];
            double t = (yt[i] != 0.0) ? 1.0 : 0.0;
            if (p < EPS) p = EPS;
            if (p > 1.0 - EPS) p = 1.0 - EPS;
            acc += t ? -log(p) : -log(1.0 - p);
        }
        result = acc / (double)n;
        break;
    }
    default: /* DYN_METRIC_ACCURACY */
        for (i = 0; i < n; i++)
            if (yt[i] == yp[i])
                acc += 1.0;
        result = acc / (double)n;
        break;
    }
    free(yt);
    free(yp);
    return JS_NewFloat64(ctx, result);
}


/* ==================================================================== *
 *  classification metrics beyond accuracy (W9.6)
 * ==================================================================== *
 *
 * accuracy alone is useless on imbalanced data -- a classifier that always
 * answers "negative" scores 0.99 on a 1%-positive set -- which is why these
 * exist. All of them are binary, with `positive` selecting the label counted as
 * positive (default 1).
 *
 * The zero-denominator convention is scikit-learn's: a precision with no
 * predicted positives is 0, not NaN, so a degenerate fold does not poison an
 * averaged score with NaN.
 */

/* Confusion counts for a binary problem. */
typedef struct { double tp, fp, tn, fn_; } dyn_binconf;

static dyn_binconf dyn_binconf_of(const double *yt, const double *yp, size_t n,
                                  double pos)
{
    dyn_binconf c = { 0, 0, 0, 0 };
    size_t i;
    for (i = 0; i < n; i++) {
        int t = (yt[i] == pos), p = (yp[i] == pos);
        if (t && p) c.tp += 1.0;
        else if (!t && p) c.fp += 1.0;
        else if (!t && !p) c.tn += 1.0;
        else c.fn_ += 1.0;
    }
    return c;
}

enum { DYN_BM_PRECISION, DYN_BM_RECALL, DYN_BM_F1, DYN_BM_SPECIFICITY,
       DYN_BM_BALANCED_ACC, DYN_BM_MCC, DYN_BM_KAPPA };

static JSValue dyn_metric_binary(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    double *yt = NULL, *yp = NULL, pos = 1.0, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "expected (yTrue, yPred[, positive])");
    /* coerce the optional label BEFORE reading the vectors, so a throwing
     * valueOf cannot strand the two malloc'd buffers */
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt); free(yp);

    switch (magic) {
    case DYN_BM_PRECISION:
        r = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
        break;
    case DYN_BM_RECALL:
        r = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        break;
    case DYN_BM_F1: {
        double p = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
        double q = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        r = (p + q) == 0.0 ? 0.0 : 2.0 * p * q / (p + q);
        break;
    }
    case DYN_BM_SPECIFICITY:
        r = (c.tn + c.fp) == 0.0 ? 0.0 : c.tn / (c.tn + c.fp);
        break;
    case DYN_BM_BALANCED_ACC: {
        double rec = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
        double spe = (c.tn + c.fp) == 0.0 ? 0.0 : c.tn / (c.tn + c.fp);
        r = 0.5 * (rec + spe);
        break;
    }
    case DYN_BM_MCC: {
        /* Matthews correlation: the one number that stays honest on a badly
         * imbalanced set, because every cell of the matrix is in it. */
        double num = c.tp * c.tn - c.fp * c.fn_;
        double den = sqrt((c.tp + c.fp) * (c.tp + c.fn_) *
                          (c.tn + c.fp) * (c.tn + c.fn_));
        r = (den == 0.0) ? 0.0 : num / den;
        break;
    }
    default: { /* DYN_BM_KAPPA -- Cohen's kappa against chance agreement */
        double tot = c.tp + c.fp + c.tn + c.fn_;
        double po, pe;
        if (tot == 0.0) { r = 0.0; break; }
        po = (c.tp + c.tn) / tot;
        pe = ((c.tp + c.fp) * (c.tp + c.fn_) +
              (c.tn + c.fn_) * (c.tn + c.fp)) / (tot * tot);
        r = (pe == 1.0) ? 0.0 : (po - pe) / (1.0 - pe);
        break;
    }
    }
    return JS_NewFloat64(ctx, r);
}

/* fbeta(yTrue, yPred, beta, positive?) -- beta > 1 weights recall. */
static JSValue dyn_metric_fbeta(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double *yt = NULL, *yp = NULL, b = 1.0, pos = 1.0, p, q, b2, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "expected (yTrue, yPred, beta[, positive])");
    if (JS_ToFloat64(ctx, &b, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &pos, argv[3]))
        return JS_EXCEPTION;
    if (!(b > 0.0))
        return JS_ThrowRangeError(ctx, "fbeta: beta must be positive");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt); free(yp);
    p = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
    q = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
    b2 = b * b;
    r = (b2 * p + q) == 0.0 ? 0.0 : (1.0 + b2) * p * q / (b2 * p + q);
    return JS_NewFloat64(ctx, r);
}

/* rocAuc(yTrue, yScore, positive?) via the rank (Mann-Whitney U) identity,
 * which is exact and O(n log n) -- no threshold sweep and no trapezoid error.
 * Ties get averaged ranks, which is what makes a constant scorer come out at
 * exactly 0.5 instead of 0 or 1. */
static int dyn_auc_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* (score, index) pairs: descending score, ascending index on ties, so the
 * order is deterministic and matches what the selection sort produced. */
typedef struct {
    double s;
    size_t i;
} dyn_ap_kv_t;

static int dyn_ap_cmp(const void *a, const void *b)
{
    const dyn_ap_kv_t *x = (const dyn_ap_kv_t *)a, *y = (const dyn_ap_kv_t *)b;
    if (x->s != y->s)
        return (x->s < y->s) ? 1 : -1;
    return (x->i > y->i) ? 1 : (x->i < y->i) ? -1 : 0;
}

static JSValue dyn_metric_auc(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int want_pr)
{
    double *yt = NULL, *ys = NULL, pos = 1.0, r = 0.0;
    size_t n, i;
    double npos = 0.0, nneg = 0.0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "expected (yTrue, yScore[, positive])");
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &ys, &n))
        return JS_EXCEPTION;
    /* NaN scores break the comparator's strict ordering (UB in qsort), and a
     * NaN label would silently count as a negative; refuse both */
    for (i = 0; i < n; i++) {
        if (!isfinite(yt[i]) || !isfinite(ys[i])) {
            free(yt); free(ys);
            return JS_ThrowRangeError(ctx,
                "rocAuc scores and labels must be finite");
        }
    }
    for (i = 0; i < n; i++) {
        if (yt[i] == pos) npos += 1.0; else nneg += 1.0;
    }
    if (npos == 0.0 || nneg == 0.0) {
        free(yt); free(ys);
        return JS_ThrowRangeError(ctx,
            "rocAuc: needs both a positive and a negative sample");
    }

    if (!want_pr) {
        /* rank-sum with tie-averaged ranks */
        double *sorted = (double *)malloc(n * sizeof(double));
        double ranksum = 0.0;
        if (!sorted) { free(yt); free(ys); return JS_ThrowOutOfMemory(ctx); }
        memcpy(sorted, ys, n * sizeof(double));
        dyn_ml_isort(sorted, n, sizeof(double), dyn_auc_cmp);
        for (i = 0; i < n; i++) {
            if (yt[i] != pos)
                continue;
            /* rank of ys[i] among `sorted`, averaged over an equal run */
            {
                size_t lo = 0, hi = n, mid, first, last;
                while (lo < hi) { mid = (lo + hi) / 2;
                    if (sorted[mid] < ys[i]) lo = mid + 1; else hi = mid; }
                first = lo;
                lo = 0; hi = n;
                while (lo < hi) { mid = (lo + hi) / 2;
                    if (sorted[mid] <= ys[i]) lo = mid + 1; else hi = mid; }
                last = lo;
                ranksum += ((double)(first + 1) + (double)last) / 2.0;
            }
        }
        free(sorted);
        r = (ranksum - npos * (npos + 1.0) / 2.0) / (npos * nneg);
    } else {
        /* average precision: sort by descending score, accumulate precision at
         * each positive. This is the step-function AP, not the interpolated
         * trapezoid -- the same choice scikit-learn's average_precision_score
         * makes, because the trapezoid is optimistically biased here. The
         * paired sort is O(n log n); the "selection by repeated max" that
         * sat here before was the O(n^2) this comment used to warn against. */
        dyn_ap_kv_t *kv = (dyn_ap_kv_t *)malloc(n * sizeof(*kv));
        double tp = 0.0, fp = 0.0, ap = 0.0;
        if (!kv) { free(yt); free(ys); return JS_ThrowOutOfMemory(ctx); }
        for (i = 0; i < n; i++) { kv[i].s = ys[i]; kv[i].i = i; }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ap_cmp);
        for (i = 0; i < n; i++) {
            if (yt[kv[i].i] == pos) { tp += 1.0; ap += tp / (tp + fp); }
            else fp += 1.0;
        }
        free(kv);
        r = ap / npos;
    }
    free(yt); free(ys);
    return JS_NewFloat64(ctx, r);
}


/* ==================================================================== *
 *  model selection: fold and split INDEX generation (W9.7)
 * ==================================================================== *
 *
 * These return INDICES, not slices of the data. That is the whole design
 * decision: dyna:ml accepts both an Array of rows and a flat Float64Array plus
 * a shape, and a splitter that returned data would have to pick one, copy the
 * matrix, and lose the zero-copy flat path that exists precisely to avoid
 * copying. Indices work against either representation and cost O(n) instead of
 * O(n * features).
 *
 * Shuffling draws from the same seeded SplitMix64 the estimators use, so a
 * given seed reproduces a given split exactly -- which is the only reason a
 * cross-validation score is comparable between two runs.
 */

/* Fisher-Yates over an index vector. Unbiased: j is drawn from [0, i), not
 * from a modulo of a fixed range. */
static void dyn_ml_shuffle_idx(uint32_t *idx, size_t n, uint64_t *state)
{
    size_t i;
    for (i = n; i > 1; i--) {
        size_t j = (size_t)(dyn_splitmix64(state) % (uint64_t)i);
        uint32_t t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
}

static JSValue dyn_ml_idx_array(JSContext *ctx, const uint32_t *v, size_t n)
{
    JSValue a = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i,
                                         JS_NewUint32(ctx, v[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

/* {train: [...], test: [...]} */
static JSValue dyn_ml_split_obj(JSContext *ctx, const uint32_t *tr, size_t ntr,
                                const uint32_t *te, size_t nte)
{
    JSValue o = JS_NewObject(ctx), a;
    if (JS_IsException(o))
        return o;
    a = dyn_ml_idx_array(ctx, tr, ntr);
    if (JS_IsException(a) ||
        JS_SetPropertyStr(ctx, o, "train", a) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    a = dyn_ml_idx_array(ctx, te, nte);
    if (JS_IsException(a) ||
        JS_SetPropertyStr(ctx, o, "test", a) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

/* Read {seed, shuffle, testSize, k} once, up front. */
static int dyn_ml_sel_opts(JSContext *ctx, JSValueConst opt, uint64_t *seed,
                           int *shuffle, double *test_size, int32_t *k)
{
    JSValue v;
    if (JS_IsUndefined(opt) || JS_IsNull(opt))
        return 0;
    if (!JS_IsObject(opt)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
#define OPT_GET(name, body) \
    do { v = JS_GetPropertyStr(ctx, opt, name); \
         if (JS_IsException(v)) return -1; \
         if (!JS_IsUndefined(v)) { body } \
         JS_FreeValue(ctx, v); } while (0)

    OPT_GET("seed", { double d; int64_t sd;
                      if (JS_ToFloat64(ctx, &d, v)) { JS_FreeValue(ctx, v); return -1; }
                      /* NaN/out-of-range in the float->int64 cast is UB */
                      if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
                          JS_ThrowRangeError(ctx, "seed must fit the int64 range");
                          JS_FreeValue(ctx, v);
                          return -1;
                      }
                      sd = (int64_t)d;
                      *seed = (uint64_t)sd; });
    OPT_GET("shuffle", { *shuffle = JS_ToBool(ctx, v); });
    if (test_size)
        OPT_GET("testSize", { if (JS_ToFloat64(ctx, test_size, v)) { JS_FreeValue(ctx, v); return -1; } });
    if (k) {
        OPT_GET("k", { if (JS_ToInt32(ctx, k, v)) { JS_FreeValue(ctx, v); return -1; } });
        /* `folds` is the word most callers reach for, and an ignored option is
         * the worst kind: {folds: 4} silently ran 5 folds and looked like it
         * worked. Unknown keys stay ignored for forward compatibility; this one
         * is not unknown, it is the same option spelled the obvious way. */
        OPT_GET("folds", { if (JS_ToInt32(ctx, k, v)) { JS_FreeValue(ctx, v); return -1; } });
    }
#undef OPT_GET
    return 0;
}

/* trainTestSplit(n, {testSize=0.25, shuffle=true, seed=12345})
 *   -> {train: idx[], test: idx[]}
 * `n` may also be an Array/TypedArray, in which case its length is used, so a
 * caller can pass y directly instead of y.length. */
static JSValue dyn_ml_train_test_split(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    uint64_t seed = 12345;
    int shuffle = 1;
    double test_size = 0.25, nd;
    uint32_t *idx = NULL;
    size_t n, ntest, i;
    JSValue r;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "trainTestSplit(n[, options])");
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                        &seed, &shuffle, &test_size, NULL) < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "trainTestSplit: needs at least 2 samples");
    if (!(test_size > 0.0 && test_size < 1.0))
        return JS_ThrowRangeError(ctx, "trainTestSplit: testSize must be in (0, 1)");
    n = (size_t)nd;
    ntest = (size_t)floor((double)n * test_size + 0.5);
    if (ntest < 1) ntest = 1;
    if (ntest > n - 1) ntest = n - 1;   /* both sides always non-empty */

    idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!idx)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);
    r = dyn_ml_split_obj(ctx, idx + ntest, n - ntest, idx, ntest);
    free(idx);
    return r;
}

/* kFold(n, {k=5, shuffle=false, seed=12345}) -> [{train, test}, ...]
 *
 * Fold sizes differ by at most one: the first n%k folds get one extra sample,
 * so every index appears in exactly one test fold and no sample is dropped --
 * asserted by the tests as a partition property rather than by fold size. */
static JSValue dyn_ml_kfold(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    uint64_t seed = 12345;
    int shuffle = 0;
    int32_t k = 5;
    double nd;
    uint32_t *idx = NULL, *tr = NULL;
    size_t n, i, f, start = 0;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "kFold(n[, options])");
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                        &seed, &shuffle, NULL, &k) < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "kFold: needs at least 2 samples");
    n = (size_t)nd;
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "kFold: k must be in [2, n]");
    /* the output is k folds of ~n indices each: O(k*n) JS array elements */
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "kFold: k * n exceeds the output index budget (20000000)");

    idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    tr = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!idx || !tr) { free(idx); free(tr); return JS_ThrowOutOfMemory(ctx); }
    for (i = 0; i < n; i++) idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) { free(idx); free(tr); return JS_EXCEPTION; }
    for (f = 0; f < (size_t)k; f++) {
        size_t sz = n / (size_t)k + (f < n % (size_t)k ? 1 : 0), ntr = 0, j;
        JSValue e;
        for (j = 0; j < n; j++)
            if (j < start || j >= start + sz)
                tr[ntr++] = idx[j];
        e = dyn_ml_split_obj(ctx, tr, ntr, idx + start, sz);
        if (JS_IsException(e) ||
            JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            free(idx); free(tr);
            return JS_EXCEPTION;
        }
        start += sz;
    }
    free(idx); free(tr);
    return out;
}

/* stratifiedKFold(y, {k=5, shuffle=false, seed=12345}) -> [{train, test}, ...]
 *
 * Each fold keeps the class proportions of the whole set, which is the point:
 * an unstratified 5-fold over a 5%-positive target can hand a fold with zero
 * positives to a classifier, and the score for that fold is meaningless.
 * Implemented by round-robining each class's members across folds, so the
 * per-class counts differ by at most one between folds. */
static JSValue dyn_ml_stratified_kfold(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    uint64_t seed = 12345;
    int shuffle = 0;
    int32_t k = 5;
    double *y = NULL;
    uint32_t *idx = NULL, *fold = NULL, *tr = NULL, *te = NULL;
    size_t n = 0, i, f;
    JSValue out, lv;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "stratifiedKFold(y[, options])");
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                        &seed, &shuffle, NULL, &k) < 0)
        return JS_EXCEPTION;
    /* read y as a vector of labels */
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "stratifiedKFold: y must be an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv))
        return JS_EXCEPTION;
    {
        double nd;
        if (JS_ToFloat64(ctx, &nd, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
        if (!(nd >= 2.0) || nd > 1e8)
            return JS_ThrowRangeError(ctx, "stratifiedKFold: needs at least 2 samples");
        n = (size_t)nd;
    }
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "stratifiedKFold: k must be in [2, n]");
    /* same output budget as kFold */
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "stratifiedKFold: k * n exceeds the output index budget (20000000)");
    y = (double *)malloc(n * sizeof(double));
    if (!y)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &y[i], e)) {
            JS_FreeValue(ctx, e); free(y); return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        /* a NaN label is its own class under != and poisons the fold
         * assignment below; reject it like every other supervised input */
        if (!isfinite(y[i])) {
            free(y);
            return JS_ThrowRangeError(ctx, "stratifiedKFold: labels must be finite");
        }
    }

    idx  = (uint32_t *)malloc(n * sizeof(uint32_t));
    fold = (uint32_t *)malloc(n * sizeof(uint32_t));
    tr   = (uint32_t *)malloc(n * sizeof(uint32_t));
    te   = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!idx || !fold || !tr || !te) {
        free(y); free(idx); free(fold); free(tr); free(te);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) { idx[i] = (uint32_t)i; fold[i] = 0; }
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    /* Round-robin within each class. Sort (label, index) pairs so each class
     * is one contiguous run -- the nested scan this replaces was O(n^2) on
     * all-distinct labels -- then hand folds 0,1,..,k-1,0,1,.. within a run.
     * The index tie-break keeps the assignment identical to encounter order. */
    {
        dyn_tree_kv_t *kv = (dyn_tree_kv_t *)malloc(n * sizeof(*kv));
        size_t run, pos, cnt;
        if (!kv) {
            free(y); free(idx); free(fold); free(tr); free(te);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++) { kv[i].key = y[idx[i]]; kv[i].row = idx[i]; }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ml_kv_cmp);
        for (run = 0; run < n; ) {
            double k0 = kv[run].key;
            cnt = 0;
            for (pos = run; pos < n && kv[pos].key == k0; pos++, cnt++)
                fold[kv[pos].row] = (uint32_t)(cnt % (size_t)k) + 1u;  /* 1-based */
            run = pos;
        }
        free(kv);
    }

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) goto fail;
    for (f = 0; f < (size_t)k; f++) {
        size_t ntr = 0, nte = 0;
        JSValue e;
        for (i = 0; i < n; i++) {
            if (fold[i] == (uint32_t)f + 1u) te[nte++] = (uint32_t)i;
            else tr[ntr++] = (uint32_t)i;
        }
        e = dyn_ml_split_obj(ctx, tr, ntr, te, nte);
        if (JS_IsException(e) ||
            JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            goto fail;
        }
    }
    free(y); free(idx); free(fold); free(tr); free(te);
    return out;
fail:
    free(y); free(idx); free(fold); free(tr); free(te);
    return JS_EXCEPTION;
}

/* confusionMatrix(yTrue, yPred) -> Array<Array<number>>, indexed [true][pred].
 * Labels must be non-negative integers; the matrix is sized by the largest label
 * seen in either vector, so classes absent from one side still get a row/column. */
static JSValue dyn_metric_confusion(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i, nc;
    double maxlab = 0.0;
    double *cm;
    JSValue out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "expected (yTrue, yPred)");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        double a = yt[i], b = yp[i];
        if (!(a >= 0.0) || !(b >= 0.0) || a != floor(a) || b != floor(b)) {
            free(yt); free(yp);
            return JS_ThrowRangeError(ctx,
                "confusionMatrix labels must be non-negative integers");
        }
        if (a > maxlab) maxlab = a;
        if (b > maxlab) maxlab = b;
    }
    if (maxlab > 4095.0) {   /* a 4096^2 matrix is already 128 MB of doubles */
        free(yt); free(yp);
        return JS_ThrowRangeError(ctx,
            "confusionMatrix supports labels up to 4095");
    }
    nc = (size_t)maxlab + 1;
    cm = (double *)calloc(nc * nc, sizeof(double));
    if (!cm) {
        free(yt); free(yp);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        cm[(size_t)yt[i] * nc + (size_t)yp[i]] += 1.0;
    free(yt);
    free(yp);
    out = dyn_ml_matrix_to_js(ctx, cm, nc, nc, 0);
    free(cm);
    return out;
}



/* ==================================================================== *
 *  W9.7 -- model selection: crossValScore, gridSearch, randomSearch     *
 * ==================================================================== *
 *
 * These drive a MODEL rather than data, so they call back into JS: fit(),
 * predict() and the constructor are all JS-visible methods. That makes them
 * unusual for this file, and it dictates the discipline:
 *
 *   - the caller's X and y are read into owned C buffers ONCE, before any
 *     callback runs, because a fit() can do anything to the JS heap;
 *   - the row subsets handed to each fold are freshly-built JS arrays, so a
 *     model that keeps a reference to its training data keeps its own copy;
 *   - nothing native is held across a JS_Call except the plain C buffers.
 *
 * `estimator` is a FACTORY, not an instance: `() => new RandomForest({...})`.
 * A search that reused one instance would be measuring a model that has
 * already seen the test fold, and cloning from C is not possible without
 * knowing every class. Making the caller write the factory makes the freshness
 * explicit rather than assumed.
 */

/* Build a JS array of the rows of `mx` selected by idx[0..n). */
static JSValue dyn_ml_rows_subset(JSContext *ctx, const dyn_matrix_t *mx,
                                  const uint32_t *idx, size_t n)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i, j;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue row = JS_NewArray(ctx);
        const double *src = mx->data + (size_t)idx[i] * mx->cols;
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        for (j = 0; j < mx->cols; j++)
            if (JS_DefinePropertyValueUint32(ctx, row, (uint32_t)j,
                    JS_NewFloat64(ctx, src[j]), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, row);
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, row,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_ml_vec_subset(JSContext *ctx, const double *v,
                                 const uint32_t *idx, size_t n)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewFloat64(ctx, v[idx[i]]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}


/* Accuracy over two JS arrays, computed in C.
 *
 * The default scorer is NOT a synthesised JS callable wrapping dyn_metric:
 * building one means casting a magic-signature function pointer to
 * JSCFunction*, and calling a function pointer through a different type is UB
 * that traps under -fsanitize=function, CFI and LTO (CLAUDE.md section 1.4).
 * It is also slower. Returns -1 (throwing) on a bad element. */
static int dyn_ml_accuracy_js(JSContext *ctx, JSValueConst yTrue,
                              JSValueConst yPred, double *out)
{
    size_t n, m, i, hit = 0;
    if (dyn_ml_len(ctx, yTrue, &n) || dyn_ml_len(ctx, yPred, &m))
        return -1;
    if (n != m) {
        JS_ThrowTypeError(ctx, "scorer: yTrue and yPred have different lengths");
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, yTrue, (uint32_t)i);
        JSValue b = JS_GetPropertyUint32(ctx, yPred, (uint32_t)i);
        double da, db;
        int bad = JS_IsException(a) || JS_IsException(b) ||
                  JS_ToFloat64(ctx, &da, a) || JS_ToFloat64(ctx, &db, b);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (bad)
            return -1;
        if (da == db)
            hit++;
    }
    *out = n ? (double)hit / (double)n : 0.0;
    return 0;
}

/* Score with the caller's function, or with accuracy when they gave none. */
static int dyn_ml_apply_scorer(JSContext *ctx, JSValueConst scorer,
                               JSValueConst yTrue, JSValueConst yPred,
                               double *out)
{
    JSValueConst a[2];
    JSValue r;
    if (!JS_IsFunction(ctx, scorer))
        return dyn_ml_accuracy_js(ctx, yTrue, yPred, out);
    a[0] = yTrue;
    a[1] = yPred;
    r = JS_Call(ctx, scorer, JS_UNDEFINED, 2, a);
    if (JS_IsException(r))
        return -1;
    if (JS_ToFloat64(ctx, out, r)) {
        JS_FreeValue(ctx, r);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

/* Score one fold: build the estimator, fit on `tr`, predict `te`, score.
 * `scorer` is a JS function (yTrue, yPred) -> number; NULL means accuracy for
 * a classifier-shaped problem and R^2 otherwise, chosen by the caller. */
static int dyn_ml_score_fold(JSContext *ctx, JSValueConst factory,
                             JSValueConst scorer, const dyn_matrix_t *mx,
                             const double *y, const uint32_t *tr, size_t ntr,
                             const uint32_t *te, size_t nte, double *out)
{
    JSValue model = JS_UNDEFINED, Xtr = JS_UNDEFINED, ytr = JS_UNDEFINED;
    JSValue Xte = JS_UNDEFINED, yte = JS_UNDEFINED, pred = JS_UNDEFINED;
    JSValue fit = JS_UNDEFINED, predict = JS_UNDEFINED, r = JS_UNDEFINED;
    JSValueConst args[2];
    int rc = -1;

    model = JS_Call(ctx, factory, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(model))
        goto done;
    if (!JS_IsObject(model)) {
        JS_ThrowTypeError(ctx, "estimator must be a factory returning a model, "
                               "e.g. () => new LogisticRegression()");
        goto done;
    }
    Xtr = dyn_ml_rows_subset(ctx, mx, tr, ntr);
    ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
    Xte = dyn_ml_rows_subset(ctx, mx, te, nte);
    yte = dyn_ml_vec_subset(ctx, y, te, nte);
    if (JS_IsException(Xtr) || JS_IsException(ytr) ||
        JS_IsException(Xte) || JS_IsException(yte))
        goto done;

    fit = JS_GetPropertyStr(ctx, model, "fit");
    predict = JS_GetPropertyStr(ctx, model, "predict");
    if (JS_IsException(fit) || JS_IsException(predict) ||
        !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
        JS_ThrowTypeError(ctx, "the estimator needs fit() and predict()");
        goto done;
    }
    args[0] = Xtr; args[1] = ytr;
    r = JS_Call(ctx, fit, model, 2, args);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);
    r = JS_UNDEFINED;
    pred = JS_Call(ctx, predict, model, 1, (JSValueConst *)&Xte);
    if (JS_IsException(pred))
        goto done;
    if (dyn_ml_apply_scorer(ctx, scorer, yte, pred, out))
        goto done;
    rc = 0;
done:
    /* close() the model if it has one: a search builds hundreds of them, and a
     * resource-backed estimator would otherwise hold every fit alive until the
     * collector caught up. Only on the success path -- a close failure must
     * not clobber the primary exception, and calling JS with one pending
     * would re-enter from an error state. */
    if (JS_IsObject(model) && !rc) {
        JSValue close = JS_GetPropertyStr(ctx, model, "close");
        if (JS_IsException(close)) {
            rc = -1;
        } else {
            if (JS_IsFunction(ctx, close)) {
                JSValue cr = JS_Call(ctx, close, model, 0, NULL);
                if (JS_IsException(cr))
                    rc = -1;
                JS_FreeValue(ctx, cr);
            }
            JS_FreeValue(ctx, close);
        }
    }
    JS_FreeValue(ctx, model); JS_FreeValue(ctx, Xtr); JS_FreeValue(ctx, ytr);
    JS_FreeValue(ctx, Xte); JS_FreeValue(ctx, yte); JS_FreeValue(ctx, pred);
    JS_FreeValue(ctx, fit); JS_FreeValue(ctx, predict); JS_FreeValue(ctx, r);
    return rc;
}

/* Read {folds, shuffle, seed, scoring} and build the fold index arrays by
 * calling the existing kFold, so there is one definition of a fold. */
static int dyn_ml_folds(JSContext *ctx, JSValueConst opts, size_t rows,
                        JSValue *out)
{
    JSValue args[2], r;
    args[0] = JS_NewInt64(ctx, (int64_t)rows);
    args[1] = JS_IsObject(opts) ? JS_DupValue(ctx, opts) : JS_UNDEFINED;
    r = dyn_ml_kfold(ctx, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r))
        return -1;
    *out = r;
    return 0;
}

/* The inverse of dyn_ml_idx_array above: a JS index array back to C. */
static int dyn_ml_read_idx(JSContext *ctx, JSValueConst arr, uint32_t **out,
                           size_t *n)
{
    int64_t len;
    size_t i;
    uint32_t *v;
    if (dyn_ml_len(ctx, arr, n))
        return -1;
    len = (int64_t)*n;
    v = (uint32_t *)malloc((*n ? *n : 1) * sizeof(uint32_t));
    if (!v) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < (size_t)len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        int64_t iv;
        if (JS_IsException(e) || JS_ToInt64(ctx, &iv, e)) {
            JS_FreeValue(ctx, e);
            free(v);
            return -1;
        }
        JS_FreeValue(ctx, e);
        v[i] = (uint32_t)iv;
    }
    *out = v;
    return 0;
}

/* crossValScore(estimator, X, y, options?) -> number[] of per-fold scores.
 *
 * The scores are returned, not the mean, because the SPREAD is the useful part:
 * 0.90 +- 0.01 and 0.90 +- 0.30 are different models and a mean hides it. */
static JSValue dyn_ml_cross_val_score(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_matrix_t mx = {0};
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    size_t nfold, f;
    (void)this_val;

    if (argc < 3 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "crossValScore(estimatorFactory, X, y[, options])");
    if (dyn_ml_ingest_Xy(ctx, argv[1], argv[2], JS_UNDEFINED, JS_UNDEFINED,
                         &mx, &y))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        scorer = JS_GetPropertyStr(ctx, opts, "scoring");
        if (JS_IsException(scorer))
            goto done;
    }
    /* No scorer => accuracy, computed in C by dyn_ml_apply_scorer. A regression
     * estimator should pass its own rather than have one guessed from y. */
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds))
        goto done;
    if (dyn_ml_len(ctx, folds, &nfold))
        goto done;
    scores = (double *)malloc((nfold ? nfold : 1) * sizeof(double));
    if (!scores) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (f = 0; f < nfold; f++) {
        JSValue fold = JS_GetPropertyUint32(ctx, folds, (uint32_t)f);
        JSValue trv, tev;
        uint32_t *tr = NULL, *te = NULL;
        size_t ntr = 0, nte = 0;
        int bad;
        if (JS_IsException(fold))
            goto done;
        trv = JS_GetPropertyStr(ctx, fold, "train");
        tev = JS_GetPropertyStr(ctx, fold, "test");
        JS_FreeValue(ctx, fold);
        bad = JS_IsException(trv) || JS_IsException(tev) ||
              dyn_ml_read_idx(ctx, trv, &tr, &ntr) ||
              dyn_ml_read_idx(ctx, tev, &te, &nte);
        JS_FreeValue(ctx, trv);
        JS_FreeValue(ctx, tev);
        if (bad || dyn_ml_score_fold(ctx, argv[0], scorer, &mx, y,
                                     tr, ntr, te, nte, &scores[f])) {
            free(tr); free(te);
            goto done;
        }
        free(tr);
        free(te);
    }
    out = dyn_ml_doubles_to_js(ctx, scores, nfold);
done:
    dyn_matrix_free(&mx);
    free(y);
    free(scores);
    JS_FreeValue(ctx, folds);
    JS_FreeValue(ctx, scorer);
    return out;
}


/* gridSearch(estimator, X, y, grid, options?) -> {best, bestScore, results}
 *
 * `estimator` takes the parameter object: `(p) => new RandomForest(p)`. That is
 * one line for the caller and it means this function needs to know nothing
 * about how any model is constructed -- no clone(), no set_params(), no
 * registry of option names.
 *
 * `grid` is `{maxDepth: [2,4,8], nEstimators: [10,50]}` and every combination
 * is tried. `results` holds every point with its per-fold scores, because the
 * runner-up and the spread are what tell you whether the winner is real.
 *
 * randomSearch is the same walk over `n` sampled points instead of the full
 * product (magic 1), because sampling beats the grid as soon as one parameter
 * matters much more than another -- the grid spends its budget re-testing the
 * irrelevant one at every level of the relevant one.
 */
static JSValue dyn_ml_grid_search(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int random)
{
    dyn_matrix_t mx = {0};
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValue results = JS_UNDEFINED, best = JS_UNDEFINED, keys = JS_UNDEFINED;
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    size_t nfold = 0, nkey = 0, combo, k, f;
    size_t *lens = NULL, *pos = NULL;
    uint64_t seed = 12345;
    int64_t n_iter = 10;
    double bestScore = -1e308;
    uint64_t total = 1;   /* the grid-size cap must not wrap on 32-bit size_t */
    (void)this_val;

    if (argc < 4 || !JS_IsFunction(ctx, argv[0]) || !JS_IsObject(argv[3]))
        return JS_ThrowTypeError(ctx, random
            ? "randomSearch(estimatorFactory, X, y, grid[, options])"
            : "gridSearch(estimatorFactory, X, y, grid[, options])");
    if (dyn_ml_ingest_Xy(ctx, argv[1], argv[2], JS_UNDEFINED, JS_UNDEFINED,
                         &mx, &y))
        return JS_EXCEPTION;

    if (JS_IsObject(opts)) {
        size_t sz = (size_t)seed;
        scorer = JS_GetPropertyStr(ctx, opts, "scoring");
        if (JS_IsException(scorer))
            goto done;
        if (dyn_opt_size(ctx, opts, "seed", &sz, 0))
            goto done;
        seed = (uint64_t)sz;
        {
            JSValue v = JS_GetPropertyStr(ctx, opts, "nIter");
            if (JS_IsException(v))
                goto done;
            if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &n_iter, v)) {
                JS_FreeValue(ctx, v);
                goto done;
            }
            JS_FreeValue(ctx, v);
        }
    }
    if (n_iter < 1) {
        JS_ThrowRangeError(ctx, "nIter must be at least 1");
        goto done;
    }
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds) || dyn_ml_len(ctx, folds, &nfold))
        goto done;

    /* the grid's own keys, in their own order -- Object.keys order is the
     * caller's declaration order, which makes `results` readable */
    {
        JSPropertyEnum *tab;
        uint32_t cnt, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, argv[3],
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            goto done;
        keys = JS_NewArray(ctx);
        if (JS_IsException(keys)) {
            for (i = 0; i < cnt; i++) JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            goto done;
        }
        for (i = 0; i < cnt; i++) {
            JSValue kn = JS_AtomToString(ctx, tab[i].atom);
            JS_FreeAtom(ctx, tab[i].atom);
            if (JS_IsException(kn) ||
                JS_DefinePropertyValueUint32(ctx, keys, i, kn, JS_PROP_C_W_E) < 0) {
                for (++i; i < cnt; i++) JS_FreeAtom(ctx, tab[i].atom);
                js_free(ctx, tab);
                goto done;
            }
        }
        js_free(ctx, tab);
        nkey = cnt;
    }
    if (nkey == 0) {
        JS_ThrowTypeError(ctx, "the grid has no parameters");
        goto done;
    }
    lens = (size_t *)malloc(nkey * sizeof(size_t));
    pos = (size_t *)malloc(nkey * sizeof(size_t));
    scores = (double *)malloc((nfold ? nfold : 1) * sizeof(double));
    if (!lens || !pos || !scores) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (k = 0; k < nkey; k++) {
        JSValue kn = JS_GetPropertyUint32(ctx, keys, (uint32_t)k);
        JSValue vals = JS_IsException(kn) ? JS_EXCEPTION
                                          : JS_GetPropertyStr(ctx, argv[3],
                                                JS_ToCString(ctx, kn));
        JS_FreeValue(ctx, kn);
        if (JS_IsException(vals) || !JS_IsArray(ctx, vals) ||
            dyn_ml_len(ctx, vals, &lens[k])) {
            JS_FreeValue(ctx, vals);
            JS_ThrowTypeError(ctx, "every grid value must be an array");
            goto done;
        }
        JS_FreeValue(ctx, vals);
        if (lens[k] == 0) {
            JS_ThrowTypeError(ctx, "a grid parameter has no values");
            goto done;
        }
        if (total > (size_t)1 << 20 || lens[k] > (size_t)1 << 20 ||
            total * lens[k] > (size_t)1 << 20) {
            JS_ThrowRangeError(ctx, "the grid has more than 2^20 points");
            goto done;
        }
        total *= lens[k];
        pos[k] = 0;
    }
    if (random && (size_t)n_iter < total)
        total = (size_t)n_iter;

    results = JS_NewArray(ctx);
    if (JS_IsException(results))
        goto done;
    for (combo = 0; combo < total; combo++) {
        JSValue params = JS_NewObject(ctx), rec, sarr;
        double mean = 0.0;
        size_t rest = combo;
        if (JS_IsException(params))
            goto done;
        for (k = 0; k < nkey; k++) {
            JSValue kn = JS_GetPropertyUint32(ctx, keys, (uint32_t)k);
            const char *kname = JS_ToCString(ctx, kn);
            JSValue vals, one;
            size_t pick;
            JS_FreeValue(ctx, kn);
            if (!kname) { JS_FreeValue(ctx, params); goto done; }
            if (random) {
                pick = (size_t)(dyn_splitmix64(&seed) % (uint64_t)lens[k]);
            } else {
                pick = rest % lens[k];
                rest /= lens[k];
            }
            vals = JS_GetPropertyStr(ctx, argv[3], kname);
            one = JS_IsException(vals) ? JS_EXCEPTION
                                       : JS_GetPropertyUint32(ctx, vals, (uint32_t)pick);
            JS_FreeValue(ctx, vals);
            if (JS_IsException(one) ||
                JS_DefinePropertyValueStr(ctx, params, kname, one, JS_PROP_C_W_E) < 0) {
                JS_FreeCString(ctx, kname);
                JS_FreeValue(ctx, params);
                goto done;
            }
            JS_FreeCString(ctx, kname);
        }
        /* one factory call per fold, with these parameters bound */
        for (f = 0; f < nfold; f++) {
            JSValue fold = JS_GetPropertyUint32(ctx, folds, (uint32_t)f);
            JSValue trv, tev;
            uint32_t *tr = NULL, *te = NULL;
            size_t ntr = 0, nte = 0;
            int bad;
            if (JS_IsException(fold)) { JS_FreeValue(ctx, params); goto done; }
            trv = JS_GetPropertyStr(ctx, fold, "train");
            tev = JS_GetPropertyStr(ctx, fold, "test");
            JS_FreeValue(ctx, fold);
            bad = JS_IsException(trv) || JS_IsException(tev) ||
                  dyn_ml_read_idx(ctx, trv, &tr, &ntr) ||
                  dyn_ml_read_idx(ctx, tev, &te, &nte);
            JS_FreeValue(ctx, trv);
            JS_FreeValue(ctx, tev);
            if (bad) { free(tr); free(te); JS_FreeValue(ctx, params); goto done; }
            {
                /* call factory(params) directly through a tiny shim: score_fold
                 * calls factory() with no arguments, so pre-apply here */
                JSValue m = JS_Call(ctx, argv[0], JS_UNDEFINED, 1,
                                    (JSValueConst *)&params);
                JSValue fit, predict, Xtr, ytr, Xte, yte, pred, r;
                JSValueConst a2[2];
                int ok = 0;
                if (JS_IsException(m)) { free(tr); free(te); JS_FreeValue(ctx, params); goto done; }
                /* the same estimator-shape guards score_fold enforces: a
                 * factory returning a non-object, or a model without fit()/
                 * predict(), must fail with a named error, not a secondary
                 * "not a function" from inside JS_Call */
                if (!JS_IsObject(m)) {
                    JS_FreeValue(ctx, m);
                    JS_ThrowTypeError(ctx,
                        "estimator must be a factory returning a model, "
                        "e.g. () => new LogisticRegression()");
                    free(tr); free(te); JS_FreeValue(ctx, params); goto done;
                }
                Xtr = dyn_ml_rows_subset(ctx, &mx, tr, ntr);
                ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
                Xte = dyn_ml_rows_subset(ctx, &mx, te, nte);
                yte = dyn_ml_vec_subset(ctx, y, te, nte);
                if (JS_IsException(Xtr) || JS_IsException(ytr) ||
                    JS_IsException(Xte) || JS_IsException(yte)) {
                    JS_FreeValue(ctx, m); JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr); JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    free(tr); free(te); JS_FreeValue(ctx, params); goto done;
                }
                fit = JS_GetPropertyStr(ctx, m, "fit");
                predict = JS_GetPropertyStr(ctx, m, "predict");
                if (JS_IsException(fit) || JS_IsException(predict) ||
                    !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
                    JS_FreeValue(ctx, fit); JS_FreeValue(ctx, predict);
                    JS_FreeValue(ctx, m); JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr); JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    JS_ThrowTypeError(ctx, "the estimator needs fit() and predict()");
                    free(tr); free(te); JS_FreeValue(ctx, params); goto done;
                }
                a2[0] = Xtr; a2[1] = ytr;
                r = JS_Call(ctx, fit, m, 2, a2);
                pred = JS_IsException(r) ? JS_EXCEPTION
                                         : JS_Call(ctx, predict, m, 1, (JSValueConst *)&Xte);
                JS_FreeValue(ctx, r);
                if (!JS_IsException(pred) &&
                    !dyn_ml_apply_scorer(ctx, scorer, yte, pred, &scores[f]))
                    ok = 1;
                JS_FreeValue(ctx, pred);
                JS_FreeValue(ctx, fit); JS_FreeValue(ctx, predict);
                JS_FreeValue(ctx, Xtr); JS_FreeValue(ctx, ytr);
                JS_FreeValue(ctx, Xte); JS_FreeValue(ctx, yte);
                if (!JS_IsException(pred)) {
                    JSValue close = JS_GetPropertyStr(ctx, m, "close");
                    if (JS_IsFunction(ctx, close)) {
                        JSValue cr = JS_Call(ctx, close, m, 0, NULL);
                        if (JS_IsException(cr))
                            ok = 0;
                        JS_FreeValue(ctx, cr);
                    }
                    JS_FreeValue(ctx, close);
                }
                JS_FreeValue(ctx, m);
                free(tr); free(te);
                if (!ok) { JS_FreeValue(ctx, params); goto done; }
            }
            mean += scores[f];
        }
        mean /= (double)(nfold ? nfold : 1);
        rec = JS_NewObject(ctx);
        sarr = dyn_ml_doubles_to_js(ctx, scores, nfold);
        if (JS_IsException(rec) || JS_IsException(sarr) ||
            JS_DefinePropertyValueStr(ctx, rec, "params", JS_DupValue(ctx, params),
                                      JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, rec, "scores", sarr, JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, rec, "mean", JS_NewFloat64(ctx, mean),
                                      JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueUint32(ctx, results, (uint32_t)combo, rec,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, params);
            goto done;
        }
        if (mean > bestScore) {
            bestScore = mean;
            JS_FreeValue(ctx, best);
            best = JS_DupValue(ctx, params);
        }
        JS_FreeValue(ctx, params);
    }
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        goto done;
    if (JS_DefinePropertyValueStr(ctx, out, "best", JS_DupValue(ctx, best),
                                  JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, out, "bestScore",
                                  JS_NewFloat64(ctx, bestScore), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, out, "results", JS_DupValue(ctx, results),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, out);
        out = JS_EXCEPTION;
    }
done:
    dyn_matrix_free(&mx);
    free(y); free(scores); free(lens); free(pos);
    JS_FreeValue(ctx, folds); JS_FreeValue(ctx, scorer);
    JS_FreeValue(ctx, results); JS_FreeValue(ctx, best); JS_FreeValue(ctx, keys);
    return out;
}

/* ---- the two explicit answers to missing data (W9.4a) --------------------
 *
 * Preprocessing, not a hidden option: both take the data and hand back new
 * data, so the line where imputation happened is visible in the caller.
 */

/* imputeMean(X) -> a new X with every non-finite entry replaced by its COLUMN
 * mean over the finite values.
 *
 * A column with no finite value has no mean, and filling it with 0 would
 * invent data, so that throws. The mean is the least-assumption filler for a
 * numeric column; anything better needs a model, which is what the rest of
 * this module is for. */
static JSValue dyn_ml_impute_mean(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_matrix_t mx = {0};
    size_t rows, cols, i, j;
    JSValue out;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    (void)this_val;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    rows = mx.rows;
    cols = mx.cols;
    /* The ingest may have aliased a caller Float64Array; imputing writes, so
     * take a private copy before touching anything. */
    if (!mx.owned) {
        double *copy = (double *)malloc(rows * cols * sizeof(double));
        if (!copy) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(copy, mx.data, rows * cols * sizeof(double));
        mx.data = copy;
        mx.owned = 1;
    }
    /* Two ROW-MAJOR passes with cols-wide accumulators, not 2*cols strided
     * ones. Walking a row-major matrix column-major pulls a 64-byte line to use
     * 8 bytes of it, and did so 2*cols times over the whole matrix; every fit
     * on real data routes through here, because a non-finite value is refused
     * and this is the named remedy. Summation order per column is unchanged --
     * i still ascends -- so the means are bit-identical. */
    {
        double *csum = (double *)calloc(cols ? cols : 1, sizeof(double));
        size_t *cn = (size_t *)calloc(cols ? cols : 1, sizeof(size_t));
        if (!csum || !cn) {
            free(csum); free(cn);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < rows; i++) {
            const double *ri = mx.data + i * cols;
            for (j = 0; j < cols; j++)
                if (isfinite(ri[j])) { csum[j] += ri[j]; cn[j]++; }
        }
        for (j = 0; j < cols; j++) {
            if (cn[j] == 0) {
                free(csum); free(cn);
                dyn_matrix_free(&mx);
                return JS_ThrowRangeError(ctx,
                    "column %u has no finite value to take a mean from",
                    (unsigned)j);
            }
            csum[j] /= (double)cn[j];      /* csum now holds the mean */
        }
        for (i = 0; i < rows; i++) {
            double *ri = mx.data + i * cols;
            for (j = 0; j < cols; j++)
                if (!isfinite(ri[j])) ri[j] = csum[j];
        }
        free(csum); free(cn);
    }
    out = dyn_ml_matrix_to_js(ctx, mx.data, rows, cols, 0);
    dyn_matrix_free(&mx);
    return out;
}

/* dropMissing(X, y?) -> { X, y, kept } with every row holding a non-finite
 * value removed, in order.
 *
 * `kept` is the surviving ROW INDICES, not just a count, so a caller can carry
 * along any other column they hold -- weights, ids, timestamps -- without
 * re-deriving which rows survived. Returning only the filtered X and y is the
 * version of this function that forces everyone to write the index loop
 * themselves. */
static JSValue dyn_ml_drop_missing(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_matrix_t mx = {0};
    double *y = NULL, *outy = NULL;
    uint32_t *keep = NULL;
    size_t rows, cols, i, j, kept = 0;
    JSValue res = JS_EXCEPTION, xs, ys, ks;
    int have_y = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
    (void)this_val;

    /* Deliberately NOT dyn_ml_ingest_Xy: that path now rejects non-finite
     * input, and this function's entire purpose is to be handed some. */
    if (dyn_ml_ingest_X(ctx, argv[0], argc > 2 ? argv[2] : JS_UNDEFINED,
                        argc > 3 ? argv[3] : JS_UNDEFINED, &mx))
        return JS_EXCEPTION;
    rows = mx.rows;
    cols = mx.cols;
    if (have_y && dyn_ml_ingest_vector(ctx, argv[1], rows, &y)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    keep = (uint32_t *)malloc((rows ? rows : 1) * sizeof(uint32_t));
    outy = (double *)malloc((rows ? rows : 1) * sizeof(double));
    if (!keep || !outy) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    {
        double *packed = (double *)malloc((rows ? rows : 1) * cols * sizeof(double));
        if (!packed) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        for (i = 0; i < rows; i++) {
            int ok = 1;
            for (j = 0; j < cols; j++)
                if (!isfinite(mx.data[i * cols + j])) { ok = 0; break; }
            if (ok && y && !isfinite(y[i]))
                ok = 0;
            if (!ok)
                continue;
            memcpy(packed + kept * cols, mx.data + i * cols, cols * sizeof(double));
            if (y) outy[kept] = y[i];
            keep[kept] = (uint32_t)i;
            kept++;
        }
        res = JS_NewObject(ctx);
        if (JS_IsException(res)) { free(packed); goto done; }
        xs = dyn_ml_matrix_to_js(ctx, packed, kept, cols, 0);
        free(packed);
        if (JS_IsException(xs)) { JS_FreeValue(ctx, res); res = JS_EXCEPTION; goto done; }
        if (JS_DefinePropertyValueStr(ctx, res, "X", xs, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res); res = JS_EXCEPTION; goto done;
        }
        ys = have_y ? dyn_ml_f64array(ctx, outy, kept) : JS_NULL;
        if (JS_IsException(ys) ||
            JS_DefinePropertyValueStr(ctx, res, "y", ys, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res); res = JS_EXCEPTION; goto done;
        }
        ks = JS_NewArray(ctx);
        if (JS_IsException(ks)) { JS_FreeValue(ctx, res); res = JS_EXCEPTION; goto done; }
        for (i = 0; i < kept; i++)
            if (JS_DefinePropertyValueUint32(ctx, ks, (uint32_t)i,
                    JS_NewUint32(ctx, keep[i]), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, ks); JS_FreeValue(ctx, res);
                res = JS_EXCEPTION; goto done;
            }
        if (JS_DefinePropertyValueStr(ctx, res, "kept", ks, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res); res = JS_EXCEPTION; goto done;
        }
    }
done:
    dyn_matrix_free(&mx);
    free(y);
    free(outy);
    free(keep);
    return res;
}

static const JSCFunctionListEntry dyn_ml_funcs[] = {
    JS_CFUNC_MAGIC_DEF("meanSquaredError", 2, dyn_metric, DYN_METRIC_MSE),
    JS_CFUNC_MAGIC_DEF("meanAbsoluteError", 2, dyn_metric, DYN_METRIC_MAE),
    JS_CFUNC_MAGIC_DEF("r2Score", 2, dyn_metric, DYN_METRIC_R2),
    JS_CFUNC_MAGIC_DEF("logLoss", 2, dyn_metric, DYN_METRIC_LOGLOSS),
    JS_CFUNC_MAGIC_DEF("accuracy", 2, dyn_metric, DYN_METRIC_ACCURACY),
    JS_CFUNC_DEF("confusionMatrix", 2, dyn_metric_confusion),
    JS_CFUNC_MAGIC_DEF("precision", 2, dyn_metric_binary, DYN_BM_PRECISION),
    JS_CFUNC_MAGIC_DEF("recall", 2, dyn_metric_binary, DYN_BM_RECALL),
    JS_CFUNC_MAGIC_DEF("f1", 2, dyn_metric_binary, DYN_BM_F1),
    JS_CFUNC_MAGIC_DEF("specificity", 2, dyn_metric_binary, DYN_BM_SPECIFICITY),
    JS_CFUNC_MAGIC_DEF("balancedAccuracy", 2, dyn_metric_binary, DYN_BM_BALANCED_ACC),
    JS_CFUNC_MAGIC_DEF("matthewsCorrcoef", 2, dyn_metric_binary, DYN_BM_MCC),
    JS_CFUNC_MAGIC_DEF("cohenKappa", 2, dyn_metric_binary, DYN_BM_KAPPA),
    JS_CFUNC_DEF("fbeta", 3, dyn_metric_fbeta),
    JS_CFUNC_DEF("trainTestSplit", 1, dyn_ml_train_test_split),
    JS_CFUNC_DEF("crossValScore", 3, dyn_ml_cross_val_score),
    JS_CFUNC_MAGIC_DEF("gridSearch", 4, dyn_ml_grid_search, 0),
    JS_CFUNC_MAGIC_DEF("randomSearch", 4, dyn_ml_grid_search, 1),
    JS_CFUNC_DEF("imputeMean", 1, dyn_ml_impute_mean),
    JS_CFUNC_DEF("dropMissing", 1, dyn_ml_drop_missing),
    JS_CFUNC_DEF("kFold", 1, dyn_ml_kfold),
    JS_CFUNC_DEF("stratifiedKFold", 1, dyn_ml_stratified_kfold),
    JS_CFUNC_MAGIC_DEF("rocAuc", 2, dyn_metric_auc, 0),
    JS_CFUNC_MAGIC_DEF("averagePrecision", 2, dyn_metric_auc, 1),

};

/* Model persistence (W9.1): the DYNS envelope, per-model codecs, and the
 * serialize/save/deserialize/load surface. A separate file only because this
 * one is already 5500 lines; it is part of this translation unit because the
 * model structs it reads are static here. */
#include "dyna-ml-persist.inc.c"

/* ---------- module registration -------------------------------------------- */


/* ==================================================================== *
 *  Pipeline (W9.7) -- scalers -> decomposition -> estimator, one fit
 *
 *  A compiled capability in the §1 sense: the STAGES are the configuration
 *  and the data goes in fit/predict. Its value is not speed -- it composes
 *  the same calls a caller would make by hand -- but that the composition
 *  cannot be got wrong. The classic error it removes is fitting the scaler
 *  on the whole dataset and only then splitting: the test fold's mean has
 *  leaked into the training statistics and the score is optimistic. Inside
 *  a Pipeline the scaler is fitted by fit() and only APPLIED by predict(),
 *  so passing the Pipeline to crossValScore does the right thing by
 *  construction.
 *
 *  The stages are held as JSValues and driven through their public JS
 *  methods, so a Pipeline composes anything with the right shape --
 *  including an estimator this module does not know about. That is why it
 *  needs a gc_mark: it holds strong references into the JS heap.
 *
 *  OWNERSHIP: a Pipeline owns its stages and closes them. That is the
 *  plan's requirement, and it makes `using p = new Pipeline([...])` release
 *  every stage. It also means a stage must not be shared between two
 *  Pipelines, which the constructor cannot detect and the docs state.
 * ==================================================================== */

typedef struct {
    JSValue *stage;
    size_t n;
    int fitted;
    /* The runtime is captured at construction so dispose() can release the
     * stage references. It has to: the framework's close() sets the
     * resource's `native` pointer to NULL after calling dispose, so a
     * finalizer that ran later would have nothing left to free -- which is
     * exactly the leak that tripped JS_FreeRuntime's gc_obj_list assertion
     * the first time this class was written. A JSRuntime outlives every
     * object in it, so holding one is safe where holding a JSContext would
     * not be. */
    JSRuntime *rt;
} dyn_pipe_t;

static JSClassID dyn_pipe_class_id;

/* dispose() IS the teardown: it is the finalizer, holds the JSRuntime, and
 * releases the stage refs here. Ownership is by reference, not cascade: an
 * earlier close()-closes-stages version broke shared stages and ran JS from
 * a teardown path. */
static void dyn_pipe_dispose(void *native)
{
    dyn_pipe_t *pp = (dyn_pipe_t *)native;
    size_t i;
    if (!pp)
        return;
    for (i = 0; i < pp->n; i++)
        JS_FreeValueRT(pp->rt, pp->stage[i]);
    free(pp->stage);
    free(pp);
}

static void dyn_pipe_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    DynResource *r = (DynResource *)JS_GetOpaque(val, dyn_pipe_class_id);
    dyn_pipe_t *pp = r ? (dyn_pipe_t *)r->native : NULL;
    size_t i;
    if (!pp || !pp->stage)
        return;
    for (i = 0; i < pp->n; i++)
        JS_MarkValue(rt, pp->stage[i], mark_func);
}

static const JSClassDef dyn_pipe_class = {
    "Pipeline", .finalizer = dyn_res_finalizer, .gc_mark = dyn_pipe_mark,
};

static JSValue dyn_pipe_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv)
{
    dyn_pipe_t *pp;
    int64_t len = 0, i;
    JSValue ret;

    (void)new_target;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new Pipeline(stages[]) requires an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    }
    if (len < 1)
        return JS_ThrowRangeError(ctx, "a Pipeline needs at least one stage");
    if (len > 64)
        return JS_ThrowRangeError(ctx, "at most 64 stages");

    pp = (dyn_pipe_t *)calloc(1, sizeof(*pp));
    if (!pp)
        return JS_ThrowOutOfMemory(ctx);
    pp->rt = JS_GetRuntime(ctx);
    pp->stage = (JSValue *)calloc((size_t)len, sizeof(JSValue));
    if (!pp->stage) {
        free(pp);
        return JS_ThrowOutOfMemory(ctx);
    }
    /* Every stage is read and type-checked BEFORE the Pipeline exists, so a
     * getter on the array cannot observe a half-built one. */
    for (i = 0; i < len; i++) {
        JSValue st = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue fn;
        if (JS_IsException(st))
            goto fail;
        if (!JS_IsObject(st)) {
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "Pipeline stage %d is not an object", (int)i);
            goto fail;
        }
        fn = JS_GetPropertyStr(ctx, st, "fit");
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "Pipeline stage %d has no fit()", (int)i);
            goto fail;
        }
        JS_FreeValue(ctx, fn);
        /* Every stage but the last must be able to transform. Checking here
         * rather than at fit() means the error names the stage while the
         * caller is still looking at the constructor. */
        if (i < len - 1) {
            fn = JS_GetPropertyStr(ctx, st, "transform");
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, st);
                JS_ThrowTypeError(ctx,
                    "Pipeline stage %d has no transform(); only the LAST stage "
                    "may be a bare estimator", (int)i);
                goto fail;
            }
            JS_FreeValue(ctx, fn);
        }
        pp->stage[i] = st;
        pp->n = (size_t)(i + 1);
    }
    ret = dyn_res_wrap(ctx, dyn_pipe_class_id, pp, dyn_pipe_dispose);
    if (JS_IsException(ret))
        return ret;   /* dyn_res_wrap ran dispose already */
    return ret;

fail:
    for (i = 0; i < (int64_t)pp->n; i++)
        JS_FreeValue(ctx, pp->stage[i]);
    free(pp->stage);
    free(pp);
    return JS_EXCEPTION;
}

/* Call `name` on `obj` with `n` arguments. */
static JSValue dyn_pipe_call(JSContext *ctx, JSValueConst obj, const char *name,
                             int n, JSValueConst *args)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name);
    JSValue r;
    if (JS_IsException(fn))
        return fn;
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return JS_ThrowTypeError(ctx, "Pipeline: a stage has no %s()", name);
    }
    r = JS_Call(ctx, fn, obj, n, args);
    JS_FreeValue(ctx, fn);
    return r;
}

static dyn_pipe_t *dyn_pipe_of(JSContext *ctx, JSValueConst t)
{
    return (dyn_pipe_t *)dyn_res_native(ctx, t, dyn_pipe_class_id);
}

static JSValue dyn_pipe_fit(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    dyn_pipe_t *pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fit(X, y?) requires X");
    /* Pipeline.fit forwards only (X, y) to the final stage, so an options
     * object handed to it would be DROPPED -- including sampleWeight. Refuse
     * it rather than ignore it: a silently-dropped option is the defect this
     * module already shipped once, and noting the hazard in a comment (which
     * is what the first version of this class did) is not the same as
     * preventing it. Set the weight on the estimator directly. */
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "Pipeline.fit (set it on the estimator)"))
        return JS_EXCEPTION;

    cur = JS_DupValue(ctx, argv[0]);
    for (i = 0; i + 1 < pp->n; i++) {
        JSValueConst a[1];
        JSValue next;
        a[0] = cur;
        /* fit THEN transform, never fitTransform on the caller's whole X and
         * never transform before fit: the intermediate stages learn only from
         * what reaches them, which is what keeps a Pipeline safe to hand to
         * crossValScore. */
        next = dyn_pipe_call(ctx, pp->stage[i], "fit", 1, a);
        if (JS_IsException(next)) { JS_FreeValue(ctx, cur); return next; }
        JS_FreeValue(ctx, next);
        next = dyn_pipe_call(ctx, pp->stage[i], "transform", 1, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(next))
            return next;
        cur = next;
    }
    {
        JSValueConst a[2];
        JSValue r;
        int n = 1;
        a[0] = cur;
        if (argc > 1) { a[1] = argv[1]; n = 2; }
        r = dyn_pipe_call(ctx, pp->stage[pp->n - 1], "fit", n, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    pp->fitted = 1;
    return JS_DupValue(ctx, this_val);
}

/* predict / predictProba / transform all push X through the leading stages and
 * differ only in what they ask the last one. magic: 0 predict, 1 predictProba,
 * 2 transform (every stage, including the last if it can). */
static JSValue dyn_pipe_apply(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic)
{
    dyn_pipe_t *pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i, upto;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "X is required");
    if (!pp->fitted)
        return JS_ThrowInternalError(ctx, "Pipeline: predict before fit");

    /* transform() means "push X through the FEATURE stages". Whether the last
     * stage counts as one depends on what it is: a Pipeline ending in PCA can
     * transform all the way through, one ending in an estimator stops before
     * it. Asking the estimator to transform and reporting its absence would be
     * technically defensible and useless -- the caller wants the features. */
    upto = pp->n - 1;
    if (magic == 2) {
        JSValue fn = JS_GetPropertyStr(ctx, pp->stage[pp->n - 1], "transform");
        int can = JS_IsFunction(ctx, fn);
        JS_FreeValue(ctx, fn);
        if (can)
            upto = pp->n;
    }
    cur = JS_DupValue(ctx, argv[0]);
    for (i = 0; i < upto; i++) {
        JSValueConst a[1];
        JSValue next;
        a[0] = cur;
        next = dyn_pipe_call(ctx, pp->stage[i], "transform", 1, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(next))
            return next;
        cur = next;
    }
    if (magic == 2)
        return cur;
    {
        JSValueConst a[1];
        JSValue r;
        a[0] = cur;
        r = dyn_pipe_call(ctx, pp->stage[pp->n - 1],
                          magic ? "predictProba" : "predict", 1, a);
        JS_FreeValue(ctx, cur);
        return r;
    }
}

static JSValue dyn_pipe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_pipe_t *pp = dyn_pipe_of(ctx, this_val);
    if (!pp)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)pp->n);
    if (magic == 1)
        return JS_NewBool(ctx, pp->fitted);
    return JS_DupValue(ctx, pp->stage[pp->n - 1]);   /* .estimator */
}

static JSValue dyn_pipe_stage(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv)
{
    dyn_pipe_t *pp = dyn_pipe_of(ctx, this_val);
    int64_t i;
    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &i, argv[0]))
        return JS_ThrowTypeError(ctx, "stage(i) requires an index");
    if (i < 0)
        i += (int64_t)pp->n;
    if (i < 0 || i >= (int64_t)pp->n)
        return JS_ThrowRangeError(ctx, "stage index out of range");
    return JS_DupValue(ctx, pp->stage[i]);
}

static const JSCFunctionListEntry dyn_pipe_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_pipe_fit),
    JS_CFUNC_MAGIC_DEF("predict", 1, dyn_pipe_apply, 0),
    JS_CFUNC_MAGIC_DEF("predictProba", 1, dyn_pipe_apply, 1),
    JS_CFUNC_MAGIC_DEF("transform", 1, dyn_pipe_apply, 2),
    JS_CFUNC_DEF("stage", 1, dyn_pipe_stage),
    JS_CGETSET_MAGIC_DEF("length", dyn_pipe_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("fitted", dyn_pipe_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("estimator", dyn_pipe_get, NULL, 2),
};

static int dyn_ml_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_class(ctx, m, &dyn_csr_class_id, &dyn_csr_class,
                           dyn_csr_proto, countof(dyn_csr_proto),
                           dyn_csr_ctor, "CSR") < 0)
        return -1;
    {   /* CSR.fromDense is a static: it builds a CSR, so it cannot be a method
         * on one. dyn_register_class has no static list, so it is installed
         * through the constructor the same way deserialize is. */
        JSValue proto = JS_GetClassProto(ctx, dyn_csr_class_id);
        JSValue ctor;
        if (JS_IsException(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsException(ctor))
            return -1;
        JS_SetPropertyFunctionList(ctx, ctor, dyn_csr_statics,
                                   countof(dyn_csr_statics));
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_class(ctx, m, &dyn_linreg_class_id, &dyn_linreg_class,
                           dyn_linreg_proto, countof(dyn_linreg_proto),
                           dyn_linreg_ctor, "LinearRegression") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_logreg_class_id, &dyn_logreg_class,
                           dyn_logreg_proto, countof(dyn_logreg_proto),
                           dyn_logreg_ctor, "LogisticRegression") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_kmeans_class_id, &dyn_kmeans_class,
                           dyn_kmeans_proto, countof(dyn_kmeans_proto),
                           dyn_kmeans_ctor, "KMeans") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_svm_class_id, &dyn_svm_class,
                           dyn_svm_proto, countof(dyn_svm_proto),
                           dyn_svm_ctor, "SVC") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gmm_class_id, &dyn_gmm_class,
                           dyn_gmm_proto, countof(dyn_gmm_proto),
                           dyn_gmm_ctor, "GaussianMixture") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtc_class_id, &dyn_dtc_class,
                           dyn_dtc_proto, countof(dyn_dtc_proto),
                           dyn_dtc_ctor, "DecisionTreeClassifier") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtr_class_id, &dyn_dtr_class,
                           dyn_dtr_proto, countof(dyn_dtr_proto),
                           dyn_dtr_ctor, "DecisionTreeRegressor") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfc_class_id, &dyn_rfc_class,
                           dyn_rfc_proto, countof(dyn_rfc_proto),
                           dyn_rfc_ctor, "RandomForestClassifier") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfr_class_id, &dyn_rfr_class,
                           dyn_rfr_proto, countof(dyn_rfr_proto),
                           dyn_rfr_ctor, "RandomForestRegressor") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbr_class_id, &dyn_gbr_class,
                           dyn_gbr_proto, countof(dyn_gbr_proto),
                           dyn_gbr_ctor, "GradientBoostingRegressor") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbc_class_id, &dyn_gbc_class,
                           dyn_gbc_proto, countof(dyn_gbc_proto),
                           dyn_gbc_ctor, "GradientBoostingClassifier") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbr_class_id, &dyn_xgbr_class,
                           dyn_xgbr_xproto, countof(dyn_xgbr_xproto),
                           dyn_xgbr_ctor, "XGBRegressor") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbc_class_id, &dyn_xgbc_class,
                           dyn_xgbc_xproto, countof(dyn_xgbc_xproto),
                           dyn_xgbc_ctor, "XGBClassifier") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pipe_class_id, &dyn_pipe_class,
                           dyn_pipe_proto, countof(dyn_pipe_proto),
                           dyn_pipe_ctor, "Pipeline") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pca_class_id, &dyn_pca_class,
                           dyn_pca_proto, countof(dyn_pca_proto),
                           dyn_pca_ctor, "PCA") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_nb_class_id, &dyn_nb_class,
                           dyn_nb_proto, countof(dyn_nb_proto),
                           dyn_nb_ctor, "GaussianNB") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_clf_class_id, &dyn_knn_clf_class,
                           dyn_knn_clf_proto, countof(dyn_knn_clf_proto),
                           dyn_knn_clf_ctor, "KNClassifier") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_reg_class_id, &dyn_knn_reg_class,
                           dyn_knn_reg_proto, countof(dyn_knn_reg_proto),
                           dyn_knn_reg_ctor, "KNRegressor") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dbscan_class_id, &dyn_dbscan_class,
                           dyn_dbscan_proto, countof(dyn_dbscan_proto),
                           dyn_dbscan_ctor, "DBScan") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_stdscaler_class_id,
                           &dyn_stdscaler_class, dyn_stdscaler_proto,
                           countof(dyn_stdscaler_proto),
                           dyn_stdscaler_ctor, "StandardScaler") < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_minmax_class_id, &dyn_minmax_class,
                           dyn_minmax_proto, countof(dyn_minmax_proto),
                           dyn_minmax_ctor, "MinMaxScaler") < 0)
        return -1;
    if (dyn_ml_install_persistence(ctx) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_ml_funcs, countof(dyn_ml_funcs));
}

int js_nat_init_ml(JSContext *ctx)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, "dyna:ml", dyn_ml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "CSR");
    JS_AddModuleExport(ctx, m, "LinearRegression");
    JS_AddModuleExport(ctx, m, "LogisticRegression");
    JS_AddModuleExport(ctx, m, "KMeans");
    JS_AddModuleExport(ctx, m, "SVC");
    JS_AddModuleExport(ctx, m, "GaussianMixture");
    JS_AddModuleExport(ctx, m, "DecisionTreeClassifier");
    JS_AddModuleExport(ctx, m, "DecisionTreeRegressor");
    JS_AddModuleExport(ctx, m, "RandomForestClassifier");
    JS_AddModuleExport(ctx, m, "RandomForestRegressor");
    JS_AddModuleExport(ctx, m, "GradientBoostingRegressor");
    JS_AddModuleExport(ctx, m, "GradientBoostingClassifier");
    JS_AddModuleExport(ctx, m, "XGBRegressor");
    JS_AddModuleExport(ctx, m, "XGBClassifier");
    JS_AddModuleExport(ctx, m, "PCA");
    JS_AddModuleExport(ctx, m, "GaussianNB");
    JS_AddModuleExport(ctx, m, "KNClassifier");
    JS_AddModuleExport(ctx, m, "KNRegressor");
    JS_AddModuleExport(ctx, m, "DBScan");
    JS_AddModuleExport(ctx, m, "Pipeline");
    JS_AddModuleExport(ctx, m, "StandardScaler");
    JS_AddModuleExport(ctx, m, "MinMaxScaler");
    return JS_AddModuleExportList(ctx, m, dyn_ml_funcs, countof(dyn_ml_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_ML */
