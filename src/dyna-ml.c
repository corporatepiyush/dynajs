#include "dyna-nat.h"
#include "cutils.h"

#include "core/dyn-prng.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_opts_strict(JSContext* ctx, JSValueConst opts,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(opts))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++) {
            if (strcmp(name, keys[k]) == 0)
                break;
        }
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const ml_logreg_keys[] = { "learningRate", "tol", "l1",
    "l2", "maxIter", "C", "penalty",
    "classWeight" };
static const char* const ml_tree_keys[] = { "nEstimators", "maxDepth",
    "minSamplesSplit",
    "minSamplesLeaf", "maxFeatures",
    "maxBins", "seed", "learningRate",
    "subsample" };
static const char* const ml_tree_xgb_extra_keys[] = { "lambda", "alpha",
    "gamma",
    "minChildWeight",
    "colsampleByTree",
    "validationFraction",
    "earlyStoppingRounds" };
static const char* const ml_svm_keys[] = { "kernel", "C", "gamma", "coef0",
    "tol", "degree", "maxIter" };
static const char* const ml_gmm_keys[] = { "seed", "maxIter", "tol",
    "regCovar" };
static const char* const ml_weights_keys[] = { "sampleWeight" };
static const char* const ml_sel_split_keys[] = { "seed", "shuffle", "testSize" };
static const char* const ml_sel_fold_keys[] = { "seed", "shuffle", "k",
    "folds" };
static const char* const ml_crossval_keys[] = { "scoring", "seed", "shuffle",
    "k", "folds", "nJobs",
    "onProgress" };
static const char* const ml_search_keys[] = { "scoring", "seed", "nIter",
    "shuffle", "k", "folds",
    "nJobs", "onProgress" };

#define DYN_LOGREG_LR 0.1
#define DYN_LOGREG_ITERS 3000
#define DYN_KMEANS_MAX_ITER 300
#define DYN_TREE_MAX_DEPTH 1024
#define DYN_ML_MAX_ITERS 100000
#define DYN_ML_MAX_EM_ITERS 10000
#define DYN_ML_MAX_TREES 100000
#define DYN_LINREG_MAX_FEATURES 4096
#define DYN_RIDGE 1e-9

#define DYN_SOLVE_MIN_PIVOT (1000.0 * 2.220446049250313e-16)

static inline double dyn_ml_dot(const double* a, const double* b, size_t n)
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

static inline double dyn_ml_sum(const double* x, size_t n)
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

static inline double dyn_ml_sqdist(const double* a, const double* b, size_t d)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, t;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= d; j += 4) {
        t = a[j] - b[j];
        s0 += t * t;
        t = a[j + 1] - b[j + 1];
        s1 += t * t;
        t = a[j + 2] - b[j + 2];
        s2 += t * t;
        t = a[j + 3] - b[j + 3];
        s3 += t * t;
    }
#endif
    for (; j < d; j++) {
        t = a[j] - b[j];
        s0 += t * t;
    }
    return (s0 + s1) + (s2 + s3);
}

static inline void dyn_ml_axpy(double* restrict y, double alpha,
    const double* restrict x, size_t n)
{
    size_t j;
    for (j = 0; j < n; j++) {
        double p = alpha * x[j];
        y[j] = y[j] + p;
    }
}

static inline void dyn_ml_scale(double* restrict out,
    const double* restrict x, double s, size_t n)
{
    size_t j;
    for (j = 0; j < n; j++)
        out[j] = x[j] * s;
}

typedef struct {
    double* data;
    size_t rows, cols;
    int owned;
} dyn_matrix_t;

static void dyn_matrix_free(dyn_matrix_t* mx)
{
    if (mx->owned)
        free(mx->data);
    mx->data = NULL;
}

static JSClassID dyn_ml_f64_class_id;

static int dyn_ml_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    if (JS_GetClassID(v) != dyn_ml_f64_class_id) {
        JS_ThrowTypeError(ctx, "dyna:ml expected a Float64Array");
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 8) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:ml expected a Float64Array");
        return -1;
    }
    if (JS_IsSharedArrayBuffer(buf)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:ml does not read a Float64Array over a SharedArrayBuffer; pass a private copy (new Float64Array(x))");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:ml typed array out of bounds");
        return -1;
    }
    *pp = (double*)(base + off);
    *pn = len / 8;
    return 0;
}

static int dyn_ml_len(JSContext* ctx, JSValueConst v, size_t* out_len)
{
    JSValue lval;
    uint32_t len;
    int ret;

    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:ml expected an Array");
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

static int dyn_ml_read_row(JSContext* ctx, JSValueConst arr, double* out,
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

static int dyn_ml_row_len(JSContext* ctx, JSValueConst row, size_t* out)
{
    if (JS_IsArray(ctx, row))
        return dyn_ml_len(ctx, row, out);
    {
        double* rp;
        return dyn_ml_get_f64(ctx, row, &rp, out);
    }
}

static int dyn_ml_read_row_generic(JSContext* ctx, JSValueConst row, double* dst,
    size_t cols)
{
    if (JS_IsArray(ctx, row)) {
        size_t rlen;
        if (dyn_ml_len(ctx, row, &rlen))
            return -1;
        if (rlen != cols) {
            JS_ThrowTypeError(ctx,
                "dyna:ml every row of X must have the same length");
            return -1;
        }
        return dyn_ml_read_row(ctx, row, dst, cols);
    }
    {
        double* rp;
        size_t rn;
        if (dyn_ml_get_f64(ctx, row, &rp, &rn))
            return -1;
        if (rn != cols) {
            JS_ThrowTypeError(ctx, "dyna:ml every row of X must have the same length");
            return -1;
        }
        memcpy(dst, rp, cols * sizeof(double));
        return 0;
    }
}

static int dyn_ml_ingest_matrix_array(JSContext* ctx, JSValueConst x,
    dyn_matrix_t* mx)
{
    size_t rows, cols, count, i;
    double* data;
    JSValue row0;
    int err;

    if (dyn_ml_len(ctx, x, &rows))
        return -1;
    if (rows == 0) {
        JS_ThrowTypeError(ctx, "dyna:ml X must have at least one row");
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
        JS_ThrowTypeError(ctx, "dyna:ml X rows must have at least one feature");
        return -1;
    }
    if (rows > SIZE_MAX / cols || (count = rows * cols) > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "dyna:ml X is too large");
        return -1;
    }
    data = (double*)malloc(count * sizeof(double));
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

static int dyn_ml_ingest_matrix_flat(JSContext* ctx, JSValueConst x,
    size_t rows, size_t cols, dyn_matrix_t* mx)
{
    double* data;
    size_t total;

    if (rows == 0 || cols == 0) {
        JS_ThrowTypeError(ctx,
            "dyna:ml a flat Float64Array X requires positive (rows, cols)");
        return -1;
    }
    if (rows > SIZE_MAX / cols) {
        JS_ThrowRangeError(ctx, "dyna:ml X is too large");
        return -1;
    }
    if (dyn_ml_get_f64(ctx, x, &data, &total))
        return -1;
    if (total != rows * cols) {
        JS_ThrowTypeError(ctx,
            "dyna:ml flat Float64Array length must equal rows*cols");
        return -1;
    }
    mx->data = data;
    mx->rows = rows;
    mx->cols = cols;
    mx->owned = 0;
    return 0;
}

static int dyn_ml_ingest_vector(JSContext* ctx, JSValueConst y, size_t expect,
    double** pout)
{
    size_t n;
    double* out;

    if (JS_IsArray(ctx, y)) {
        if (dyn_ml_len(ctx, y, &n))
            return -1;
        if (n > SIZE_MAX / sizeof(double)) {
            JS_ThrowRangeError(ctx, "dyna:ml vector length out of range");
            return -1;
        }
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "dyna:ml y length must equal the number of rows in X");
            return -1;
        }
        out = (double*)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_ml_read_row(ctx, y, out, n)) {
            free(out);
            return -1;
        }
    } else {
        double* src;
        if (dyn_ml_get_f64(ctx, y, &src, &n))
            return -1;
        if (n != expect) {
            JS_ThrowTypeError(ctx,
                "dyna:ml y length must equal the number of rows in X");
            return -1;
        }
        out = (double*)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(out, src, n * sizeof(double));
    }
    *pout = out;
    return 0;
}

static int dyn_ml_ingest_any_vector(JSContext* ctx, JSValueConst v,
    double** pout, size_t* pn)
{
    size_t n;
    double* out;

    if (JS_IsArray(ctx, v)) {
        if (dyn_ml_len(ctx, v, &n))
            return -1;
        if (n > SIZE_MAX / sizeof(double)) {
            JS_ThrowRangeError(ctx, "dyna:ml vector length out of range");
            return -1;
        }
        out = (double*)malloc((n ? n : 1) * sizeof(double));
        if (!out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (dyn_ml_read_row(ctx, v, out, n)) {
            free(out);
            return -1;
        }
    } else {
        double* src;
        if (dyn_ml_get_f64(ctx, v, &src, &n))
            return -1;
        out = (double*)malloc((n ? n : 1) * sizeof(double));
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

static size_t dyn_ml_bad_cells(const dyn_matrix_t* mx, const double* y,
    int missing_ok)
{
    size_t n = mx->rows * mx->cols, i, bad = 0;
    const double* d = mx->data;

    if (missing_ok) {
        for (i = 0; i < n; i++)
            bad += (fabs(d[i]) == HUGE_VAL);
    } else {
        for (i = 0; i < n; i++)
            bad += !(d[i] > -HUGE_VAL && d[i] < HUGE_VAL);
    }
    if (y)
        for (i = 0; i < mx->rows; i++)
            bad += !(y[i] > -HUGE_VAL && y[i] < HUGE_VAL);
    return bad;
}

static int dyn_ml_check_finite_ex(JSContext* ctx, const dyn_matrix_t* mx,
    const double* y, int missing_ok)
{
    size_t rows = mx->rows, cols = mx->cols, i, j;
    const double* d = mx->data;

    if (!dyn_ml_bad_cells(mx, y, missing_ok))
        return 0;

    for (i = 0; i < rows; i++) {
        for (j = 0; j < cols; j++) {
            if (missing_ok && isnan(d[i * cols + j]))
                continue;
            if (!isfinite(d[i * cols + j])) {
                JS_ThrowRangeError(ctx,
                    "dyna:ml X[%u][%u] is %s; fit rejects missing data. Use "
                    "imputeMean(X) or dropMissing(X, y) first.",
                    (unsigned)i, (unsigned)j,
                    isnan(d[i * cols + j]) ? "NaN" : "infinite");
                return -1;
            }
        }
        if (y && !isfinite(y[i])) {
            JS_ThrowRangeError(ctx,
                "dyna:ml y[%u] is %s; a target cannot be imputed, so drop the row. "
                "Use dropMissing(X, y).",
                (unsigned)i, isnan(y[i]) ? "NaN" : "infinite");
            return -1;
        }
    }
    return 0;
}

static int dyn_ml_check_finite(JSContext* ctx, const dyn_matrix_t* mx,
    const double* y)
{
    return dyn_ml_check_finite_ex(ctx, mx, y, 0);
}

static int dyn_ml_check_finite_fast(JSContext* ctx, const dyn_matrix_t* mx)
{
    size_t n = mx->rows * mx->cols, i;
    const double* d = mx->data;
    size_t bad = 0;

    for (i = 0; i < n; i++)
        bad += !(d[i] > -HUGE_VAL && d[i] < HUGE_VAL);
    if (!bad)
        return 0;
    JS_ThrowRangeError(ctx,
        "ml.predict: predict input contains NaN or infinite values");
    return -1;
}

static JSValueConst dyn_ml_opts(JSContext* ctx, int argc, JSValueConst* argv,
    int from)
{
    int i;
    for (i = from; i < argc; i++) {
        if (!JS_IsObject(argv[i]) || JS_IsArray(ctx, argv[i]))
            continue;
        if (JS_GetBufferKind(argv[i]) != JS_BUFFER_KIND_NONE)
            continue;
        return argv[i];
    }
    return JS_UNDEFINED;
}

static int dyn_ml_ingest_weights(JSContext* ctx, int argc, JSValueConst* argv,
    int from, size_t rows, double** pw)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue wv;
    double* w = NULL;
    size_t i;
    double sum = 0.0;

    *pw = NULL;
    if (JS_IsUndefined(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, ml_weights_keys, 1))
        return -1;
    wv = JS_GetPropertyStr(ctx, opts, "sampleWeight");
    if (JS_IsException(wv))
        return -1;
    if (JS_IsUndefined(wv) || JS_IsNull(wv)) {
        JS_FreeValue(ctx, wv);
        return 0;
    }
    if (dyn_ml_ingest_vector(ctx, wv, rows, &w)) {
        JS_FreeValue(ctx, wv);
        JS_ThrowTypeError(ctx, "ml.fit: sampleWeight must have one entry per row of X");
        return -1;
    }
    JS_FreeValue(ctx, wv);

    for (i = 0; i < rows; i++) {
        if (!isfinite(w[i]) || w[i] < 0.0) {
            const char* why = isnan(w[i]) ? "NaN"
                                          : (w[i] < 0.0 ? "negative" : "infinite");
            free(w);
            JS_ThrowRangeError(ctx,
                "ml.fit: sampleWeight[%u] is %s; weights must be finite and non-negative",
                (unsigned)i, why);
            return -1;
        }
        sum += w[i];
    }
    if (rows && (!isfinite(sum) || sum <= 0.0)) {
        free(w);
        JS_ThrowRangeError(ctx,
            "ml.fit: sampleWeight sums to %s; weights must be finite and have a "
            "positive sum",
            isfinite(sum) ? "zero" : "infinity");
        return -1;
    }
    *pw = w;
    return 0;
}

static int dyn_ml_reject_weights(JSContext* ctx, int argc, JSValueConst* argv,
    int from, const char* what)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue wv;
    int present;

    if (JS_IsUndefined(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, ml_weights_keys, 1))
        return -1;
    wv = JS_GetPropertyStr(ctx, opts, "sampleWeight");
    if (JS_IsException(wv))
        return -1;
    present = !JS_IsUndefined(wv) && !JS_IsNull(wv);
    JS_FreeValue(ctx, wv);
    if (present) {
        JS_ThrowTypeError(ctx,
            "ml.fit: %s has no weighted fit, so sampleWeight would be ignored. "
            "Resample the rows instead, or use an estimator that supports it: "
            "LinearRegression, LogisticRegression, every tree model, "
            "XGBRegressor, XGBClassifier, KMeans, GaussianNB, StandardScaler.",
            what);
        return -1;
    }
    return 0;
}

static JSClassID dyn_csr_class_id;

static int dyn_ml_refuse_csr(JSContext* ctx, JSValueConst v)
{
    if (JS_GetClassID(v) != dyn_csr_class_id)
        return 0;
    JS_ThrowTypeError(ctx,
        "dyna:ml this method has no sparse path, so it cannot take a CSR. "
        "Every predict/predictProba does, and LinearRegression and "
        "LogisticRegression fit sparsely too; for anything else pass "
        "X.toDense() -- explicitly, because expanding a sparse matrix is the "
        "memory the sparse form exists to avoid.");
    return -1;
}

static int dyn_ml_ingest_Xy_ex(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** py, double** pw,
    int missing_ok)
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
        if (pw && dyn_ml_ingest_weights(ctx, argc, argv, opt_from, mx->rows, pw)) {
            dyn_matrix_free(mx);
            free(*py);
            *py = NULL;
            return -1;
        }
        if (dyn_ml_check_finite_ex(ctx, mx, *py, missing_ok)) {
            dyn_matrix_free(mx);
            free(*py);
            *py = NULL;
            if (pw) {
                free(*pw);
                *pw = NULL;
            }
            return -1;
        }
        return 0;
    } else {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) || JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "dyna:ml flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        if (dyn_ml_ingest_vector(ctx, yv, (size_t)rows64, py))
            return -1;
        if (pw && dyn_ml_ingest_weights(ctx, argc, argv, opt_from, (size_t)rows64, pw)) {
            free(*py);
            *py = NULL;
            return -1;
        }
        if (dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                (size_t)cols64, mx)) {
            if (pw) {
                free(*pw);
                *pw = NULL;
            }
            free(*py);
            *py = NULL;
            return -1;
        }
        if (dyn_ml_check_finite_ex(ctx, mx, *py, missing_ok)) {
            dyn_matrix_free(mx);
            free(*py);
            *py = NULL;
            if (pw) {
                free(*pw);
                *pw = NULL;
            }
            return -1;
        }
        return 0;
    }
}

static int dyn_ml_ingest_Xy(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** py, double** pw)
{
    return dyn_ml_ingest_Xy_ex(ctx, xv, yv, rows_arg, cols_arg,
        argc, argv, opt_from, mx, py, pw, 0);
}

static int dyn_ml_ingest_Xy_no_w(JSContext* ctx, JSValueConst xv,
    JSValueConst yv, JSValueConst rows_arg,
    JSValueConst cols_arg, dyn_matrix_t* mx,
    double** py)
{
    return dyn_ml_ingest_Xy_ex(ctx, xv, yv, rows_arg, cols_arg,
        0, NULL, 0, mx, py, NULL, 0);
}

static int dyn_ml_ingest_X(JSContext* ctx, JSValueConst xv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    dyn_matrix_t* mx)
{
    if (dyn_ml_refuse_csr(ctx, xv))
        return -1;
    if (JS_IsArray(ctx, xv))
        return dyn_ml_ingest_matrix_array(ctx, xv, mx);
    {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) || JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "dyna:ml flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        return dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
            (size_t)cols64, mx);
    }
}

static int dyn_ml_ingest_X_w(JSContext* ctx, JSValueConst xv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** pw)
{
    if (dyn_ml_refuse_csr(ctx, xv))
        return -1;
    if (JS_IsArray(ctx, xv)) {
        if (dyn_ml_ingest_matrix_array(ctx, xv, mx))
            return -1;
        if (dyn_ml_ingest_weights(ctx, argc, argv, opt_from, mx->rows, pw)) {
            dyn_matrix_free(mx);
            return -1;
        }
        return 0;
    }
    {
        int64_t rows64, cols64;
        if (JS_ToInt64(ctx, &rows64, rows_arg) || JS_ToInt64(ctx, &cols64, cols_arg))
            return -1;
        if (rows64 <= 0 || cols64 <= 0) {
            JS_ThrowTypeError(ctx,
                "dyna:ml flat Float64Array X requires positive (rows, cols) args");
            return -1;
        }
        if (dyn_ml_ingest_weights(ctx, argc, argv, opt_from,
                (size_t)rows64, pw))
            return -1;
        if (dyn_ml_ingest_matrix_flat(ctx, xv, (size_t)rows64,
                (size_t)cols64, mx)) {
            if (pw) {
                free(*pw);
                *pw = NULL;
            }
            return -1;
        }
        return 0;
    }
}

static JSValue dyn_ml_f64array(JSContext* ctx, const double* v, size_t n)
{
    static const double zero_stub = 0.0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (n == 0)
        v = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)v, n * sizeof(double));
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_ml_matrix_to_js(JSContext* ctx, const double* v, size_t rows,
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
            if (JS_DefinePropertyValueUint32(ctx, row, (uint32_t)j,
                    JS_NewFloat64(ctx, v[i * cols + j]), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, row);
                JS_FreeValue(ctx, out);
                return JS_EXCEPTION;
            }
        }
        if (JS_DefinePropertyValueUint32(ctx, out, (uint32_t)i, row, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

static JSValue dyn_ml_doubles_to_js(JSContext* ctx, const double* v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewFloat64(ctx, v[i]), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static const char* const ml_as_keys[] = { "as" };

static int dyn_ml_parse_as_opt(JSContext* ctx, int argc, JSValueConst* argv,
    int from, int* pas_f64)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue v;
    size_t len;
    const char* sv;
    int ok = 0;

    *pas_f64 = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, ml_as_keys, 1))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "as");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    sv = JS_ToCStringLen(ctx, &len, v);
    if (!sv) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    ok = strcmp(sv, "f64") == 0;
    JS_FreeCString(ctx, sv);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "ml.predict: the \"as\" option is \"f64\" (a "
                               "Float64Array result) or omitted");
        return -1;
    }
    *pas_f64 = 1;
    return 0;
}

static int dyn_ml_bind_out(JSContext* ctx, JSValueConst v, size_t expect,
    double** pp)
{
    double* p;
    size_t n;

    if (dyn_ml_get_f64(ctx, v, &p, &n))
        return -1;
    if (n < expect) {
        JS_ThrowRangeError(ctx,
            "ml.predictInto: out has %u elements but the prediction writes %u; "
            "nothing was written",
            (unsigned)n, (unsigned)expect);
        return -1;
    }
    *pp = p;
    return 0;
}

static JSValue dyn_ml_predict_result(JSContext* ctx, const double* v, size_t n,
    int as_f64)
{
    return as_f64 ? dyn_ml_f64array(ctx, v, n) : dyn_ml_doubles_to_js(ctx, v, n);
}

static void dyn_ml_into_args(int into, int argc, JSValueConst* argv,
    JSValueConst* xa, JSValueConst* rows_arg,
    JSValueConst* cols_arg)
{
    if (into) {
        *xa = argc > 1 ? argv[1] : JS_UNDEFINED;
        *rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
        *cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    } else {
        *xa = argc > 0 ? argv[0] : JS_UNDEFINED;
        *rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
        *cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    }
}

static JSValue dyn_ml_ints_to_js(JSContext* ctx, const int* v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewInt32(ctx, v[i]), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

typedef int (*dyn_ml_cmp_fn)(const void*, const void*);

static void dyn_ml_sort_swap(char* a, char* b, size_t w, char* tmp)
{
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

static void dyn_ml_hsort(char* a, size_t n, size_t w, dyn_ml_cmp_fn cmp)
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

static void dyn_ml_isort_r(char* a, size_t n, size_t w, dyn_ml_cmp_fn cmp,
    size_t depth, size_t limit)
{
    char tmp[32];
    size_t i, j;
    while (n > 16) {
        if (depth >= limit) {
            dyn_ml_hsort(a, n, w, cmp);
            return;
        }
        {
            char *x = a, *y = a + (n / 2) * w, *z = a + (n - 1) * w;
            if (cmp(y, x) < 0)
                dyn_ml_sort_swap(x, y, w, tmp);
            if (cmp(z, y) < 0)
                dyn_ml_sort_swap(y, z, w, tmp);
            if (cmp(y, x) < 0)
                dyn_ml_sort_swap(x, y, w, tmp);
            dyn_ml_sort_swap(a, y, w, tmp);
        }
        {
            size_t lo = 1, hi = n;
            for (;;) {
                while (lo < n && cmp(a + lo * w, a) < 0)
                    lo++;
                do {
                    hi--;
                } while (cmp(a + hi * w, a) > 0);
                if (lo >= hi)
                    break;
                dyn_ml_sort_swap(a + lo * w, a + hi * w, w, tmp);
                lo++;
            }
            dyn_ml_sort_swap(a, a + hi * w, w, tmp);
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
    for (i = 1; i < n; i++) {
        memcpy(tmp, a + i * w, w);
        for (j = i; j > 0 && cmp(tmp, a + (j - 1) * w) < 0; j--)
            memcpy(a + j * w, a + (j - 1) * w, w);
        memcpy(a + j * w, tmp, w);
    }
}

static void dyn_ml_isort(void* base, size_t n, size_t w, dyn_ml_cmp_fn cmp)
{
    size_t limit = 0, t;
    if (n < 2)
        return;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_isort_r((char*)base, n, w, cmp, 0, 2 * limit + 1);
}

static int dyn_ml_dbl_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1
                                  : 0;
}

static size_t dyn_ml_classes(JSContext* ctx, const double* y, size_t rows,
    double** out)
{
    double* c = (double*)malloc((rows ? rows : 1) * sizeof(double));
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
            double* rest = (double*)malloc(r * sizeof(double));
            double* merged = rest ? (double*)malloc(rows * sizeof(double)) : NULL;
            if (!rest || !merged) {
                free(rest);
                free(merged);
                free(c);
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
                else {
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

static int dyn_solve(double* A, double* b, size_t p, double min_pivot)
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
        if (!(maxv > 0.0) || !isfinite(maxv) || maxv < min_pivot)
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

typedef struct {
    double* val;
    uint32_t* col;
    size_t* ptr;
    size_t rows, cols, nnz;
} dyn_csr_t;

static void dyn_csr_dispose(void* native)
{
    dyn_csr_t* s = (dyn_csr_t*)native;
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

static double dyn_csr_dot(const double* val, const uint32_t* col, size_t nz,
    const double* dense)
{
    double s = 0.0;
    size_t k;
    for (k = 0; k < nz; k++)
        s += val[k] * dense[col[k]];
    return s;
}

static void dyn_csr_axpy(double* dense, double a, const double* val,
    const uint32_t* col, size_t nz)
{
    size_t k;
    for (k = 0; k < nz; k++)
        dense[col[k]] += a * val[k];
}

typedef struct {
    uint32_t col;
    double val;
    size_t pos;
} dyn_csr_ent_t;

static int dyn_csr_ent_cmp(const void* a, const void* b)
{
    const dyn_csr_ent_t* x = (const dyn_csr_ent_t*)a;
    const dyn_csr_ent_t* y = (const dyn_csr_ent_t*)b;
    if (x->col != y->col)
        return x->col < y->col ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static size_t dyn_csr_max_row(const dyn_csr_t* S)
{
    size_t i, m = 0;
    for (i = 0; i < S->rows; i++)
        if (S->ptr[i + 1] - S->ptr[i] > m)
            m = S->ptr[i + 1] - S->ptr[i];
    return m;
}

static long dyn_csr_row_reading(const dyn_csr_t* S, size_t i, dyn_csr_ent_t* ent,
    int* bad_nan)
{
    size_t k0 = S->ptr[i], k1 = S->ptr[i + 1], k, m = 0, w, r;
    int descending = 0;

    for (k = k0; k + 1 < k1; k++)
        if (S->col[k] > S->col[k + 1]) {
            descending = 1;
            break;
        }
    for (k = k0; k < k1; k++) {
        ent[m].col = S->col[k];
        ent[m].val = S->val[k];
        ent[m].pos = k - k0;
        m++;
    }
    if (descending)
        qsort(ent, m, sizeof(*ent), dyn_csr_ent_cmp);
    w = 0;
    r = 0;
    while (r < m) {
        uint32_t c = ent[r].col;
        double s = 0.0;
        while (r < m && ent[r].col == c) {
            s += ent[r].val;
            r++;
        }
        if (!isfinite(s)) {
            *bad_nan = isnan(s) != 0;
            return -1;
        }
        if (s != 0.0) {
            ent[w].col = c;
            ent[w].val = s;
            w++;
        }
    }
    return (long)w;
}

static JSValue dyn_csr_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_csr_t* s;
    double *val = NULL, *colf = NULL, *ptrf = NULL;
    int64_t cols64 = 0;
    size_t nnz, ncol, nptr, i;

    if (argc < 4)
        return JS_ThrowTypeError(ctx,
            "ml.CSR: new CSR(values, columns, rowPointers, cols) requires four arguments");
    if (dyn_ml_ingest_any_vector(ctx, argv[0], &val, &nnz))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_any_vector(ctx, argv[1], &colf, &ncol)) {
        free(val);
        return JS_EXCEPTION;
    }
    if (ncol != nnz) {
        free(val);
        free(colf);
        return JS_ThrowTypeError(ctx,
            "ml.CSR: columns has %u entries but values has %u; there is one column "
            "index per value",
            (unsigned)ncol, (unsigned)nnz);
    }
    if (dyn_ml_ingest_any_vector(ctx, argv[2], &ptrf, &nptr)) {
        free(val);
        free(colf);
        return JS_EXCEPTION;
    }
    if (JS_ToInt64(ctx, &cols64, argv[3])) {
        free(val);
        free(colf);
        free(ptrf);
        return JS_EXCEPTION;
    }
    if (cols64 <= 0 || nptr < 1) {
        free(val);
        free(colf);
        free(ptrf);
        return JS_ThrowRangeError(ctx,
            "ml.CSR: cols must be positive and rowPointers must have at least one entry");
    }
    s = (dyn_csr_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(val);
        free(colf);
        free(ptrf);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (nnz > SIZE_MAX / sizeof(uint32_t) || nptr > SIZE_MAX / sizeof(size_t)) {
        free(val);
        free(colf);
        free(ptrf);
        free(s);
        return JS_ThrowRangeError(ctx, "ml.CSR: allocation size overflow");
    }
    s->rows = nptr - 1;
    s->cols = (size_t)cols64;
    s->nnz = nnz;
    s->val = val;
    s->col = (uint32_t*)malloc((nnz ? nnz : 1) * sizeof(uint32_t));
    s->ptr = (size_t*)malloc(nptr * sizeof(size_t));
    if (!s->col || !s->ptr) {
        free(colf);
        free(ptrf);
        dyn_csr_dispose(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < nnz; i++) {
        if (!(colf[i] >= 0.0) || colf[i] >= (double)s->cols || colf[i] != (double)(uint32_t)colf[i] || !isfinite(val[i])) {
            unsigned long long ncols = (unsigned long long)s->cols;
            free(colf);
            free(ptrf);
            dyn_csr_dispose(s);
            return JS_ThrowRangeError(ctx,
                "ml.CSR: columns[%llu] must be an integer in [0, %llu) and its value finite",
                (unsigned long long)i, ncols);
        }
        s->col[i] = (uint32_t)colf[i];
    }
    for (i = 0; i < nptr; i++) {
        double v = ptrf[i];
        if (!(v >= 0.0) || v > (double)nnz || v != (double)(size_t)v || (i > 0 && (size_t)v < s->ptr[i - 1])) {
            free(colf);
            free(ptrf);
            dyn_csr_dispose(s);
            return JS_ThrowRangeError(ctx,
                "ml.CSR: rowPointers must be non-decreasing integers in [0, nnz]; "
                "entry %u is not",
                (unsigned)i);
        }
        s->ptr[i] = (size_t)v;
    }
    if (s->ptr[0] != 0 || s->ptr[nptr - 1] != nnz) {
        free(colf);
        free(ptrf);
        dyn_csr_dispose(s);
        return JS_ThrowRangeError(ctx,
            "ml.CSR: rowPointers must start at 0 and end at the value count (%u)",
            (unsigned)nnz);
    }
    free(colf);
    free(ptrf);
    return dyn_res_wrap(ctx, new_target, dyn_csr_class_id, s, dyn_csr_dispose);
}

static JSValue dyn_csr_from_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* s;
    size_t i, j, nnz = 0, at = 0;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    (void)this_val;
    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    for (i = 0; i < mx.rows; i++) {
        for (j = 0; j < mx.cols; j++) {
            double v = mx.data[i * mx.cols + j];
            if (!isfinite(v)) {
                dyn_matrix_free(&mx);
                return JS_ThrowRangeError(ctx,
                    "ml.CSR: fromDense keeps finite values only; X[%u][%u] is %s",
                    (unsigned)i, (unsigned)j,
                    isnan(v) ? "NaN" : "infinite");
            }
            if (v != 0.0)
                nnz++;
        }
    }
    s = (dyn_csr_t*)calloc(1, sizeof(*s));
    if (!s) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->rows = mx.rows;
    s->cols = mx.cols;
    s->nnz = nnz;
    s->val = (double*)malloc((nnz ? nnz : 1) * sizeof(double));
    s->col = (uint32_t*)malloc((nnz ? nnz : 1) * sizeof(uint32_t));
    s->ptr = (size_t*)malloc((mx.rows + 1) * sizeof(size_t));
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
    return dyn_res_wrap(ctx, JS_UNDEFINED, dyn_csr_class_id, s, dyn_csr_dispose);
}

static int dyn_ml_dense_fits(size_t rows, size_t cols)
{
    const size_t budget = (size_t)1 << 30;
    const size_t per_cell = 24;
    const size_t per_row = 280;

    if (!cols)
        cols = 1;
    if (!rows)
        rows = 1;
    if (cols > (budget - per_row) / per_cell)
        return 0;
    return rows <= budget / (per_cell * cols + per_row);
}

static JSValue dyn_csr_to_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_csr_t* s = (dyn_csr_t*)dyn_res_native(ctx, this_val, dyn_csr_class_id);
    double* out;
    size_t i, k;
    JSValue r;

    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->rows && !dyn_ml_dense_fits(s->rows, s->cols))
        return JS_ThrowRangeError(ctx, "ml.toDense: the dense form does not fit in memory");
    out = (double*)calloc(s->rows * s->cols != 0 ? s->rows * s->cols : 1,
        sizeof(double));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < s->rows; i++)
        for (k = s->ptr[i]; k < s->ptr[i + 1]; k++)
            out[i * s->cols + s->col[k]] += s->val[k];
    r = dyn_ml_matrix_to_js(ctx, out, s->rows, s->cols, 0);
    free(out);
    return r;
}

static JSValue dyn_csr_row(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_csr_t* s;
    int64_t iv;
    double* out;
    size_t k;
    JSValue r;

    if (argc < 1 || JS_ToInt64(ctx, &iv, argv[0]))
        return JS_ThrowTypeError(ctx, "ml.row: row(i) requires an index");
    s = (dyn_csr_t*)dyn_res_native(ctx, this_val, dyn_csr_class_id);
    if (!s)
        return JS_EXCEPTION;
    if (iv < 0 || (uint64_t)iv >= (uint64_t)s->rows)
        return JS_ThrowRangeError(ctx, "ml.row: row %lld is outside [0, %llu)",
            (long long)iv, (unsigned long long)s->rows);
    if (!dyn_ml_dense_fits(1, s->cols))
        return JS_ThrowRangeError(ctx,
            "ml.row: the dense row does not fit in memory");
    out = (double*)calloc(s->cols ? s->cols : 1, sizeof(double));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (k = s->ptr[iv]; k < s->ptr[iv + 1]; k++)
        out[s->col[k]] += s->val[k];
    r = dyn_ml_doubles_to_js(ctx, out, s->cols);
    free(out);
    return r;
}

#define DYN_CSR_GETTER(name, expr)                                       \
    static JSValue dyn_csr_##name(JSContext* ctx, JSValueConst this_val) \
    {                                                                    \
        dyn_csr_t* s = (dyn_csr_t*)dyn_res_native(ctx, this_val,         \
            dyn_csr_class_id);                                           \
        if (!s)                                                          \
            return JS_EXCEPTION;                                         \
        return (expr);                                                   \
    }
DYN_CSR_GETTER(rows_get, JS_NewInt64(ctx, (int64_t)s->rows))
DYN_CSR_GETTER(cols_get, JS_NewInt64(ctx, (int64_t)s->cols))
DYN_CSR_GETTER(nnz_get, JS_NewInt64(ctx, (int64_t)s->nnz))
DYN_CSR_GETTER(density_get,
    JS_NewFloat64(ctx, (s->rows && s->cols) ? (double)s->nnz / ((double)s->rows * (double)s->cols) : 0.0))

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

static dyn_csr_t* dyn_ml_as_csr(JSContext* ctx, JSValueConst v)
{
    if (JS_GetClassID(v) != dyn_csr_class_id)
        return NULL;
    return (dyn_csr_t*)dyn_res_native(ctx, v, dyn_csr_class_id);
}

static int dyn_ml_check_csr_finite(JSContext* ctx, const dyn_csr_t* S,
    const double* y)
{
    dyn_csr_ent_t* ent;
    size_t cap = dyn_csr_max_row(S), i;
    int rc = 0;

    ent = (dyn_csr_ent_t*)malloc((cap ? cap : 1) * sizeof(*ent));
    if (!ent) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < S->rows; i++) {
        int bad_nan = 0;
        if (dyn_csr_row_reading(S, i, ent, &bad_nan) < 0) {
            JS_ThrowRangeError(ctx,
                "ml.fit: X contains a %s value; fit rejects missing data. Use "
                "imputeMean(X) or dropMissing(X, y) first.",
                bad_nan ? "NaN" : "infinite");
            rc = -1;
            break;
        }
        if (!isfinite(y[i])) {
            JS_ThrowRangeError(ctx,
                "ml.fit: y[%u] is %s; a target cannot be imputed, so drop the row. "
                "Use dropMissing(X, y).",
                (unsigned)i, isnan(y[i]) ? "NaN" : "infinite");
            rc = -1;
            break;
        }
    }
    free(ent);
    return rc;
}

typedef struct {
    const double* dense;
    const dyn_csr_t* S;
    double* scratch;
    size_t rows, cols;
} dyn_ml_row_src;

static int dyn_ml_row_src_init(JSContext* ctx, dyn_ml_row_src* rs,
    const dyn_matrix_t* mx, const dyn_csr_t* S)
{
    rs->dense = mx->data;
    rs->S = S;
    rs->scratch = NULL;
    rs->rows = S ? S->rows : mx->rows;
    rs->cols = S ? S->cols : mx->cols;
    if (S) {
        if (!dyn_ml_dense_fits(1, S->cols)) {
            JS_ThrowRangeError(ctx,
                "ml.predict: a dense row of this CSR does not fit in memory");
            return -1;
        }
        rs->scratch = (double*)calloc(S->cols ? S->cols : 1, sizeof(double));
        if (!rs->scratch) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    return 0;
}

static const double* dyn_ml_row_at(const dyn_ml_row_src* rs, size_t i)
{
    if (!rs->S)
        return rs->dense + i * rs->cols;
    {
        double* x = rs->scratch;
        size_t p;
        memset(x, 0, rs->cols * sizeof(double));
        for (p = rs->S->ptr[i]; p < rs->S->ptr[i + 1]; p++)
            x[rs->S->col[p]] += rs->S->val[p];
        return x;
    }
}

static void dyn_ml_row_src_free(dyn_ml_row_src* rs)
{
    free(rs->scratch);
    rs->scratch = NULL;
}

static int dyn_ml_check_csr_values_finite(JSContext* ctx, const dyn_csr_t* S)
{
    dyn_csr_ent_t* ent;
    size_t cap = dyn_csr_max_row(S), i;

    ent = (dyn_csr_ent_t*)malloc((cap ? cap : 1) * sizeof(*ent));
    if (!ent) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < S->rows; i++) {
        int bad_nan;
        if (dyn_csr_row_reading(S, i, ent, &bad_nan) < 0) {
            free(ent);
            JS_ThrowRangeError(ctx,
                "ml.predict: predict input contains NaN or infinite values");
            return -1;
        }
    }
    free(ent);
    return 0;
}

typedef struct {
    int fitted;
    size_t n_features;
    double* coef;
    double intercept;
} dyn_linreg_t;

static JSClassID dyn_linreg_class_id;

static void dyn_linreg_dispose(void* native)
{
    dyn_linreg_t* m = (dyn_linreg_t*)native;
    if (m) {
        free(m->coef);
        free(m);
    }
}

static const JSClassDef dyn_linreg_class = {
    "LinearRegression",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_linreg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_linreg_t* m;

    (void)argc;
    (void)argv;
    m = (dyn_linreg_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    return dyn_res_wrap(ctx, new_target, dyn_linreg_class_id, m, dyn_linreg_dispose);
}

static int dyn_linreg_solve(JSContext* ctx, dyn_linreg_t* m, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* sw, const dyn_csr_t* S)
{
    size_t p = cols + 1;
    double *AtA, *Aty, *coef;
    dyn_csr_ent_t* ent = NULL;
    size_t i, a, bcol;

    if (cols > DYN_LINREG_MAX_FEATURES) {
        JS_ThrowRangeError(ctx, "ml.fit: LinearRegression solves the normal "
                                "equations, cubic in the feature count; at most "
                                "%u features",
            (unsigned)DYN_LINREG_MAX_FEATURES);
        return -1;
    }
    AtA = (double*)calloc(p * p, sizeof(double));
    Aty = (double*)calloc(p, sizeof(double));
    coef = (double*)malloc(cols * sizeof(double));
    if (!AtA || !Aty || !coef) {
        free(AtA);
        free(Aty);
        free(coef);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (S) {
        size_t entcap = dyn_csr_max_row(S);
        ent = (dyn_csr_ent_t*)malloc((entcap ? entcap : 1) * sizeof(*ent));
        if (!ent) {
            free(AtA);
            free(Aty);
            free(coef);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    if (S) {
        for (i = 0; i < S->rows; i++) {
            int bad_nan = 0;
            long n = dyn_csr_row_reading(S, i, ent, &bad_nan);
            double wi = sw ? sw[i] : 1.0;
            long ka, kb;
            if (n < 0) {
                free(AtA);
                free(Aty);
                free(coef);
                free(ent);
                JS_ThrowRangeError(ctx,
                    "ml.fit: X contains a %s value; fit rejects missing data. Use "
                    "imputeMean(X) or dropMissing(X, y) first.",
                    bad_nan ? "NaN" : "infinite");
                return -1;
            }
            for (ka = 0; ka < n; ka++) {
                size_t acol = ent[ka].col;
                double va = wi * ent[ka].val;
                Aty[acol] += va * y[i];
                for (kb = ka; kb < n; kb++) {
                    double pr = va * ent[kb].val;
                    AtA[acol * p + ent[kb].col] += pr;
                }
                AtA[acol * p + cols] += va;
            }
            Aty[cols] += wi * y[i];
            AtA[cols * p + cols] += wi;
        }
    } else if (!sw) {
        for (i = 0; i < rows; i++) {
            const double* xi = X + i * cols;
            for (a = 0; a < cols; a++) {
                double va = xi[a];
                Aty[a] += va * y[i];
                dyn_ml_axpy(AtA + a * p + a, va, xi + a, cols - a);
                AtA[a * p + cols] += va;
            }
            Aty[cols] += y[i];
            AtA[cols * p + cols] += 1.0;
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double* xi = X + i * cols;
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
    {
        double ascale = 0.0, *Asave, *bsave;

        for (a = 0; a < p; a++) {
            for (bcol = a + 1; bcol < p; bcol++)
                AtA[bcol * p + a] = AtA[a * p + bcol];
            if (AtA[a * p + a] > ascale)
                ascale = AtA[a * p + a];
        }
        Asave = (double*)malloc((size_t)p * p * sizeof(double));
        bsave = (double*)malloc(p * sizeof(double));
        if (!Asave || !bsave) {
            free(Asave);
            free(bsave);
            free(AtA);
            free(Aty);
            free(coef);
            free(ent);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(Asave, AtA, (size_t)p * p * sizeof(*Asave));
        memcpy(bsave, Aty, p * sizeof(*bsave));
        if (dyn_solve(AtA, Aty, p, ascale * DYN_SOLVE_MIN_PIVOT) != 0) {
            double wscale = 1.0;
            memcpy(AtA, Asave, (size_t)p * p * sizeof(*AtA));
            memcpy(Aty, bsave, p * sizeof(*Aty));
            if (sw && rows) {
                double wmax = 0.0, wsum = 0.0;
                for (i = 0; i < rows; i++) {
                    if (sw[i] > wmax)
                        wmax = sw[i];
                }
                if (wmax > 0.0 && isfinite(wmax)) {
                    for (i = 0; i < rows; i++)
                        wsum += sw[i] / wmax;
                    wscale = wmax * (wsum / (double)rows);
                }
            }
            if (wscale != 1.0) {
                for (a = 0; a < p; a++) {
                    for (bcol = 0; bcol < p; bcol++)
                        AtA[a * p + bcol] /= wscale;
                    Aty[a] /= wscale;
                }
            }
            for (a = 0; a < p; a++)
                AtA[a * p + a] += DYN_RIDGE;
            if (dyn_solve(AtA, Aty, p, 0.0)) {
                free(Asave);
                free(bsave);
                free(AtA);
                free(Aty);
                free(coef);
                free(ent);
                JS_ThrowInternalError(ctx,
                    "ml.fit: LinearRegression: singular system");
                return -1;
            }
        }
        free(Asave);
        free(bsave);
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
    free(ent);
    return 0;
}

static JSValue dyn_linreg_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_linreg_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *sw = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        dyn_csr_t* S;
        size_t nrows;
        if (dyn_ml_ingest_any_vector(ctx, argv[1], &y, &nrows))
            return JS_EXCEPTION;
        if (dyn_ml_ingest_weights(ctx, argc, argv, 2, nrows, &sw)) {
            free(y);
            return JS_EXCEPTION;
        }
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        if (S->rows != nrows) {
            free(y);
            free(sw);
            return JS_ThrowTypeError(ctx,
                "ml.fit: y has %u entries but X has %u rows",
                (unsigned)nrows, (unsigned)S->rows);
        }
        if (dyn_ml_check_csr_finite(ctx, S, y)) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        m = (dyn_linreg_t*)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
        if (!m) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        if (dyn_linreg_solve(ctx, m, NULL, y, S->rows, S->cols, sw, S)) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        free(y);
        free(sw);
        return JS_DupValue(ctx, this_val);
    }
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg,
            argc, argv, 2, &mx, &y, &sw))
        return JS_EXCEPTION;
    m = (dyn_linreg_t*)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        free(sw);
        return JS_EXCEPTION;
    }
    if (dyn_linreg_solve(ctx, m, mx.data, y, mx.rows, mx.cols, sw, NULL)) {
        dyn_matrix_free(&mx);
        free(y);
        free(sw);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    free(sw);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_linreg_coef_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_linreg_t* m = (dyn_linreg_t*)dyn_res_native(ctx, this_val,
        dyn_linreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->coef, m->n_features);
}

static JSValue dyn_linreg_intercept_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_linreg_t* m = (dyn_linreg_t*)dyn_res_native(ctx, this_val,
        dyn_linreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->intercept : 0.0);
}

static JSValue dyn_linreg_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int into)
{
    dyn_linreg_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* yout = NULL;
    size_t i, rows;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_linreg_t*)dyn_res_native(ctx, this_val, dyn_linreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &yout)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        yout = (double*)malloc(rows * sizeof(double));
        if (!yout) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(yout);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++)
        yout[i] = m->intercept + dyn_ml_dot(m->coef, dyn_ml_row_at(&rs, i), m->n_features);
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_predict_result(ctx, yout, rows, as_f64);
    free(yout);
    return result;
}

static JSValue dyn_linreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_linreg_predict_impl(ctx, this_val, argc, argv, 0);
}

static JSValue dyn_linreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_linreg_predict_impl(ctx, this_val, argc, argv, 1);
}

static const JSCFunctionListEntry dyn_linreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_linreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_linreg_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_linreg_predict_into),
    JS_CGETSET_DEF("coef", dyn_linreg_coef_get, NULL),
    JS_CGETSET_DEF("intercept", dyn_linreg_intercept_get, NULL),
};

typedef struct {
    int fitted;
    size_t n_features;
    size_t n_classes;
    size_t n_out;
    double* classes;
    double* coef;
    double* intercept;
    double lr, l1, l2, tol;
    size_t max_iter;
    int balanced;
    double C;
    int have_C;
    size_t n_iter;
    int converged;
} dyn_logreg_t;

static JSClassID dyn_logreg_class_id;

static void dyn_logreg_dispose(void* native)
{
    dyn_logreg_t* m = (dyn_logreg_t*)native;
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

static int dyn_opt_size(JSContext* ctx, JSValueConst obj, const char* name,
    size_t* out, size_t minimum);
static int dyn_opt_double(JSContext* ctx, JSValueConst obj, const char* name,
    double* out, double lo, double hi);

static JSValue dyn_logreg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_logreg_t* m;

    double lr = DYN_LOGREG_LR, tol = 1e-4, l1 = 0.0, l2 = 0.0, C = 1.0;
    size_t max_iter = DYN_LOGREG_ITERS;
    int balanced = 0, have_C = 0;
    const char* penalty = NULL;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        JSValue v;
        if (dyn_opts_strict(ctx, o, ml_logreg_keys, 8))
            return JS_EXCEPTION;
        if (dyn_opt_double(ctx, o, "learningRate", &lr, 1e-12, 1e12) || dyn_opt_double(ctx, o, "tol", &tol, 0.0, 1e12) || dyn_opt_double(ctx, o, "l1", &l1, 0.0, 1e12) || dyn_opt_double(ctx, o, "l2", &l2, 0.0, 1e12) || dyn_opt_size(ctx, o, "maxIter", &max_iter, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, o, "C");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (JS_ToFloat64(ctx, &C, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!(C >= 1e-12 && C <= 1e12)) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "dyna:ml %s must be in [%g, %g]", "C",
                    1e-12, 1e12);
                return JS_EXCEPTION;
            }
            have_C = 1;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "penalty");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            penalty = JS_ToCString(ctx, v);
            if (!penalty) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, o, "classWeight");
        if (JS_IsException(v)) {
            if (penalty)
                JS_FreeCString(ctx, penalty);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* cwname = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!cwname) {
                if (penalty)
                    JS_FreeCString(ctx, penalty);
                return JS_EXCEPTION;
            }
            if (!strcmp(cwname, "balanced"))
                balanced = 1;
            else {
                JS_ThrowTypeError(ctx,
                    "ml.LogisticRegression: classWeight must be \"balanced\" or absent");
                JS_FreeCString(ctx, cwname);
                if (penalty)
                    JS_FreeCString(ctx, penalty);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, cwname);
        } else {
            JS_FreeValue(ctx, v);
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "ml.LogisticRegression: expected an options object");
    }
    if (max_iter > DYN_ML_MAX_ITERS)
        return JS_ThrowRangeError(ctx, "ml.LogisticRegression: maxIter must be at most %u",
            (unsigned)DYN_ML_MAX_ITERS);
    if (penalty) {
        if (!strcmp(penalty, "l2"))
            l2 = have_C ? 1.0 / C : 1.0;
        else if (!strcmp(penalty, "l1"))
            l1 = have_C ? 1.0 / C : 1.0;
        else if (!strcmp(penalty, "elasticnet")) {
            l1 = l2 = (have_C ? 1.0 / C : 1.0) * 0.5;
        } else if (strcmp(penalty, "none") != 0) {
            JS_ThrowTypeError(ctx, "ml.LogisticRegression: penalty must be \"l1\", \"l2\", "
                                   "\"elasticnet\" or \"none\"");
            JS_FreeCString(ctx, penalty);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, penalty);
    }

    m = (dyn_logreg_t*)calloc(1, sizeof(*m));
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
    return dyn_res_wrap(ctx, new_target, dyn_logreg_class_id, m, dyn_logreg_dispose);
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

static int dyn_logreg_train_w(JSContext* ctx, dyn_logreg_t* m, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* sw, const dyn_csr_t* S)
{
    double *w = NULL, *gw = NULL, *b = NULL, *gb = NULL, *classes = NULL;
    double *cw = NULL, *p = NULL;
    size_t n_classes, n_out, i, j, k, it;
    int rc = -1;
    double wsum = 0.0;
    double l1p, l2p;

    n_classes = dyn_ml_classes(ctx, y, rows, &classes);
    if (n_classes == 0)
        return -1;
    if (n_classes < 2) {
        free(classes);
        JS_ThrowTypeError(ctx, "ml.fit: LogisticRegression needs at least two classes "
                               "in y, found %u",
            (unsigned)n_classes);
        return -1;
    }
    n_out = (n_classes == 2) ? 1 : n_classes;
    if (n_classes > 256) {
        free(classes);
        JS_ThrowRangeError(ctx,
            "ml.fit: LogisticRegression supports at most 256 classes, found %u",
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

    uint8_t* rowlab = NULL;
    double* roww = NULL;

    w = (double*)calloc(n_out * cols, sizeof(double));
    gw = (double*)malloc(n_out * cols * sizeof(double));
    b = (double*)calloc(n_out, sizeof(double));
    gb = (double*)malloc(n_out * sizeof(double));
    cw = (double*)malloc(n_classes * sizeof(double));
    p = (double*)malloc(n_out * sizeof(double));
    rowlab = (uint8_t*)malloc(rows ? rows : 1);
    roww = (double*)malloc((rows ? rows : 1) * sizeof(double));
    if (!w || !gw || !b || !gb || !cw || !p || !rowlab || !roww) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }

    for (k = 0; k < n_classes; k++)
        cw[k] = 1.0;
    if (m->balanced) {
        for (k = 0; k < n_classes; k++) {
            size_t cnt = 0;
            for (i = 0; i < rows; i++)
                if (y[i] == classes[k])
                    cnt++;
            cw[k] = cnt ? (double)rows / ((double)n_classes * (double)cnt) : 1.0;
        }
    }

    if (sw) {
        for (i = 0; i < rows; i++)
            wsum += sw[i];
    } else {
        wsum = (double)rows;
    }

    {
        size_t r_;
        for (r_ = 0; r_ < rows; r_++) {
            size_t lab = 0;
            for (k = 0; k < n_classes; k++)
                if (y[r_] == classes[k]) {
                    lab = k;
                    break;
                }
            rowlab[r_] = (uint8_t)lab;
            roww[r_] = sw ? cw[lab] * sw[r_] : cw[lab];
        }
    }

    for (it = 0; it < m->max_iter; it++) {
        double maxgrad = 0.0;
        if (JS_CheckInterrupt(ctx))
            goto done;
        for (j = 0; j < n_out * cols; j++)
            gw[j] = 0.0;
        for (k = 0; k < n_out; k++)
            gb[k] = 0.0;

        for (i = 0; i < rows; i++) {
            const double* xi = S ? NULL : X + i * cols;
            size_t label = rowlab[i];
            double weight = roww[i];
            if (S) {
                const double* sv = S->val + S->ptr[i];
                const uint32_t* sc = S->col + S->ptr[i];
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
                        if (k == 0 || p[k] > mx)
                            mx = p[k];
                    }
                    for (k = 0; k < n_out; k++) {
                        p[k] = exp(p[k] - mx);
                        sum += p[k];
                    }
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
                double mx = 0.0, sum = 0.0;
                for (k = 0; k < n_out; k++) {
                    p[k] = b[k] + dyn_ml_dot(w + k * cols, xi, cols);
                    if (k == 0 || p[k] > mx)
                        mx = p[k];
                }
                for (k = 0; k < n_out; k++) {
                    p[k] = exp(p[k] - mx);
                    sum += p[k];
                }
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
                if (l1p != 0.0) {
                    double t = m->lr * l1p;
                    v = v > t ? v - t : (v < -t ? v + t : 0.0);
                }
                if (l2p != 0.0)
                    v /= 1.0 + m->lr * l2p;
                w[idx] = v;
                {
                    double eff = (before - v) / m->lr;
                    if (fabs(eff) > maxgrad)
                        maxgrad = fabs(eff);
                }
            }
            {
                double g = gb[k] / wsum;
                b[k] -= m->lr * g;
                if (fabs(g) > maxgrad)
                    maxgrad = fabs(g);
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
    w = NULL;
    b = NULL;
    classes = NULL;
    rc = 0;
done:
    free(w);
    free(gw);
    free(b);
    free(gb);
    free(cw);
    free(p);
    free(classes);
    free(rowlab);
    free(roww);
    return rc;
}

static void dyn_logreg_scores(const dyn_logreg_t* m, const double* xi,
    size_t cols, double* out)
{
    size_t k;
    for (k = 0; k < m->n_out; k++)
        out[k] = m->intercept[k] + dyn_ml_dot(m->coef + k * cols, xi, cols);
}

static JSValue dyn_logreg_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_logreg_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *sw = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        dyn_csr_t* S;
        size_t nrows;
        if (dyn_ml_ingest_any_vector(ctx, argv[1], &y, &nrows))
            return JS_EXCEPTION;
        if (dyn_ml_ingest_weights(ctx, argc, argv, 2, nrows, &sw)) {
            free(y);
            return JS_EXCEPTION;
        }
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        if (S->rows != nrows) {
            free(y);
            free(sw);
            return JS_ThrowTypeError(ctx,
                "ml.fit: y has %u entries but X has %u rows",
                (unsigned)nrows, (unsigned)S->rows);
        }
        if (dyn_ml_check_csr_finite(ctx, S, y)) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        m = (dyn_logreg_t*)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
        if (!m) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        if (dyn_logreg_train_w(ctx, m, NULL, y, S->rows, S->cols, sw, S)) {
            free(y);
            free(sw);
            return JS_EXCEPTION;
        }
        free(y);
        free(sw);
        return JS_DupValue(ctx, this_val);
    }
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg,
            argc, argv, 2, &mx, &y, &sw))
        return JS_EXCEPTION;
    m = (dyn_logreg_t*)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        free(sw);
        return JS_EXCEPTION;
    }
    if (dyn_logreg_train_w(ctx, m, mx.data, y, mx.rows, mx.cols, sw, NULL)) {
        dyn_matrix_free(&mx);
        free(y);
        free(sw);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    free(sw);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_logreg_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int proba,
    int into)
{
    dyn_logreg_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double *yout = NULL, *z = NULL;
    size_t rows, i, k, K;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_logreg_t*)dyn_res_native(ctx, this_val, dyn_logreg_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    K = m->n_classes;
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &yout)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        yout = (double*)malloc(rows * (proba ? K : 1) * sizeof(double));
        if (!yout) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    z = (double*)malloc(m->n_out * sizeof(double));
    if (!z) {
        if (!into)
            free(yout);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        free(z);
        if (!into)
            free(yout);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        dyn_logreg_scores(m, dyn_ml_row_at(&rs, i), m->n_features, z);
        if (m->n_out == 1) {
            if (proba) {
                double p1 = dyn_sigmoid(z[0]);
                yout[i * K + 0] = 1.0 - p1;
                yout[i * K + 1] = p1;
            } else {
                yout[i] = m->classes[z[0] > 0.0 ? 1 : 0];
            }
        } else {
            size_t best = 0;
            for (k = 1; k < m->n_out; k++)
                if (z[k] > z[best])
                    best = k;
            if (!proba) {
                yout[i] = m->classes[best];
            } else {
                double mxz = z[best], sum = 0.0;
                for (k = 0; k < m->n_out; k++) {
                    z[k] = exp(z[k] - mxz);
                    sum += z[k];
                }
                for (k = 0; k < m->n_out; k++)
                    yout[i * K + k] = z[k] / sum;
            }
        }
    }
    dyn_ml_row_src_free(&rs);
    free(z);
    dyn_matrix_free(&mx);
    if (into) {
        return JS_NewInt64(ctx, (int64_t)rows);
    }
    result = proba ? dyn_ml_matrix_to_js(ctx, yout, rows, K, 0)
                   : dyn_ml_predict_result(ctx, yout, rows, as_f64);
    free(yout);
    return result;
}

static JSValue dyn_logreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0, 0);
}

static JSValue dyn_logreg_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 1, 0);
}

static JSValue dyn_logreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0, 1);
}

static JSValue dyn_logreg_get_classes(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_ThrowInternalError(ctx, "ml.classes: classes before fit");
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}
static JSValue dyn_logreg_get_niter(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->n_iter);
}
static JSValue dyn_logreg_get_converged(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, m->converged);
}
static JSValue dyn_logreg_get_coef(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_ThrowInternalError(ctx, "ml.coef: coef before fit");
    return dyn_ml_matrix_to_js(ctx, m->coef, m->n_out, m->n_features, 0);
}
static JSValue dyn_logreg_get_intercept(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_ThrowInternalError(ctx, "ml.intercept: intercept before fit");
    return m->n_out == 1 ? JS_NewFloat64(ctx, m->intercept[0])
                         : dyn_ml_doubles_to_js(ctx, m->intercept, m->n_out);
}

static const JSCFunctionListEntry dyn_logreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_logreg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_logreg_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_logreg_predict_into),
    JS_CFUNC_DEF("predictProba", 1, dyn_logreg_predict_proba),
    JS_CGETSET_DEF("classes", dyn_logreg_get_classes, NULL),
    JS_CGETSET_DEF("coef", dyn_logreg_get_coef, NULL),
    JS_CGETSET_DEF("intercept", dyn_logreg_get_intercept, NULL),
    JS_CGETSET_DEF("nIter", dyn_logreg_get_niter, NULL),
    JS_CGETSET_DEF("converged", dyn_logreg_get_converged, NULL),
};

typedef struct {
    int fitted;
    size_t k;
    size_t n_features;
    uint64_t seed;
    double* centroids;
    double inertia;
} dyn_kmeans_t;

static JSClassID dyn_kmeans_class_id;

static void dyn_kmeans_dispose(void* native)
{
    dyn_kmeans_t* m = (dyn_kmeans_t*)native;
    if (m) {
        free(m->centroids);
        free(m);
    }
}

static const JSClassDef dyn_kmeans_class = {
    "KMeans",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_kmeans_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_kmeans_t* m;
    size_t k = 8;
    uint64_t seed = 0x9e3779b97f4a7c15ULL;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        int32_t kv;
        if (JS_ToInt32(ctx, &kv, argv[0]))
            return JS_EXCEPTION;
        if (kv < 1)
            return JS_ThrowRangeError(ctx, "ml.KMeans: nClusters must be >= 1");
        k = (size_t)kv;
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        JSValue seed_val = JS_DupValue(ctx, argv[1]);
        int32_t sv;
        if (JS_IsObject(seed_val)) {
            JS_FreeValue(ctx, seed_val);
            seed_val = JS_GetPropertyStr(ctx, argv[1], "seed");
            if (JS_IsException(seed_val))
                return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(seed_val)) {
            if (!JS_IsNumber(seed_val)) {
                JS_FreeValue(ctx, seed_val);
                return JS_ThrowTypeError(ctx, "ml.KMeans: seed must be a number or { seed: number }");
            }
            if (JS_ToInt32(ctx, &sv, seed_val)) {
                JS_FreeValue(ctx, seed_val);
                return JS_EXCEPTION;
            }
            if (sv >= 0)
                seed = (uint64_t)sv + 0x9e3779b97f4a7c15ULL;
        }
        JS_FreeValue(ctx, seed_val);
    }
    m = (dyn_kmeans_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = k;
    m->seed = seed;
    return dyn_res_wrap(ctx, new_target, dyn_kmeans_class_id, m, dyn_kmeans_dispose);
}

static double dyn_km_assign(const double* X, size_t rows, size_t cols,
    const double* cent, size_t k, int* labels,
    const double* w)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
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

static void dyn_km_plusplus(const double* X, size_t rows, size_t cols,
    size_t k, double* cent, double* nearest,
    uint64_t* rng)
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
            chosen = (size_t)(dyn_splitmix64(rng) % rows);
        } else {
            target = ((double)(dyn_splitmix64(rng) >> 11) * (1.0 / 9007199254740992.0)) * sum;
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

static int dyn_kmeans_train(JSContext* ctx, dyn_kmeans_t* m, const double* X,
    size_t rows, size_t cols, const double* w)
{
    size_t k = m->k, i, c, it;
    double *cent = NULL, *sums = NULL, *nearest = NULL;
    int *labels = NULL, *prev = NULL;
    double* counts = NULL;
    double last = 0.0;
    uint64_t rng = m->seed;

    if (rows < k)
        return JS_ThrowRangeError(ctx,
                   "ml.fit: KMeans needs at least nClusters rows"),
               -1;
    if (k > 64 && k > rows / 2)
        return JS_ThrowRangeError(ctx,
                   "ml.fit: KMeans nClusters too close to the number of rows"),
               -1;
    cent = (double*)malloc(k * cols * sizeof(double));
    sums = (double*)malloc(k * cols * sizeof(double));
    nearest = (double*)malloc(rows * sizeof(double));
    labels = (int*)malloc(rows * sizeof(int));
    prev = (int*)malloc(rows * sizeof(int));
    counts = (double*)malloc(k * sizeof(double));
    if (!cent || !sums || !nearest || !labels || !prev || !counts) {
        free(cent);
        free(sums);
        free(nearest);
        free(labels);
        free(prev);
        free(counts);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    dyn_km_plusplus(X, rows, cols, k, cent, nearest, &rng);
    for (i = 0; i < rows; i++)
        prev[i] = -1;
    for (it = 0; it < DYN_KMEANS_MAX_ITER; it++) {
        int changed = 0;
        if (JS_CheckInterrupt(ctx)) {
            free(cent);
            free(sums);
            free(nearest);
            free(labels);
            free(prev);
            free(counts);
            return -1;
        }
        last = dyn_km_assign(X, rows, cols, cent, k, labels, w);
        for (i = 0; i < rows; i++)
            if (labels[i] != prev[i]) {
                changed = 1;
                break;
            }
        if (!changed && it > 0)
            break;
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
            if (counts[c] <= 0.0) {
                size_t r = (size_t)(dyn_splitmix64(&rng) % rows);
                memcpy(cent + c * cols, X + r * cols, cols * sizeof(double));
            } else {
                dyn_ml_scale(cent + c * cols, sums + c * cols,
                    1.0 / counts[c], cols);
            }
        }
        memcpy(prev, labels, rows * sizeof(int));
    }
    m->inertia = (it == DYN_KMEANS_MAX_ITER)
        ? dyn_km_assign(X, rows, cols, cent, k, labels, w)
        : last;
    free(m->centroids);
    m->centroids = cent;
    m->n_features = cols;
    m->fitted = 1;
    free(sums);
    free(nearest);
    free(labels);
    free(prev);
    free(counts);
    return 0;
}

static JSValue dyn_kmeans_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_kmeans_t* m;
    dyn_matrix_t mx = { 0 };
    double* w = NULL;
    JSValueConst rows_arg, cols_arg;

    rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    if (dyn_ml_ingest_X_w(ctx, argv[0], rows_arg, cols_arg, argc, argv, 1,
            &mx, &w))
        return JS_EXCEPTION;
    m = (dyn_kmeans_t*)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    if (dyn_kmeans_train(ctx, m, mx.data, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(w);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_kmeans_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int into)
{
    dyn_kmeans_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    int* labels = NULL;
    double* yout = NULL;
    size_t rows, i;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_kmeans_t*)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    labels = (int*)malloc(rows * sizeof(int));
    if (!labels) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &yout)) {
            free(labels);
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        yout = (double*)malloc(rows * sizeof(double));
        if (!yout) {
            free(labels);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        free(labels);
        if (!into)
            free(yout);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++)
        dyn_km_assign(dyn_ml_row_at(&rs, i), 1, rs.cols,
            m->centroids, m->k, labels + i, NULL);
    for (i = 0; i < rows; i++)
        yout[i] = (double)labels[i];
    free(labels);
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_predict_result(ctx, yout, rows, as_f64);
    free(yout);
    return result;
}

static JSValue dyn_kmeans_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_kmeans_predict_impl(ctx, this_val, argc, argv, 0);
}

static JSValue dyn_kmeans_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_kmeans_predict_impl(ctx, this_val, argc, argv, 1);
}

static JSValue dyn_kmeans_inertia(JSContext* ctx, JSValueConst this_val)
{
    dyn_kmeans_t* m = (dyn_kmeans_t*)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->inertia : 0.0);
}

static const JSCFunctionListEntry dyn_kmeans_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_kmeans_fit),
    JS_CFUNC_DEF("predict", 1, dyn_kmeans_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_kmeans_predict_into),
    JS_CGETSET_DEF("inertia", dyn_kmeans_inertia, NULL),
};

#define DYN_TREE_LEAF ((uint32_t)0xffffffffu)

#define DYN_TNODE_MISSING_LEFT 1

typedef struct {
    uint32_t left, right;
    int32_t feature;
    int32_t flags;
    double threshold;
    double value;
} dyn_tnode_t;

typedef struct {
    dyn_tnode_t* node;
    size_t n_nodes, cap;
    double* improve;
    double* nsamp;
    double* proba;
    size_t n_classes;
} dyn_tree_t;

typedef struct {
    size_t max_depth;
    size_t min_samples_split;
    size_t min_samples_leaf;
    size_t max_features;
    size_t max_bins;
    int classifier;
    int newton;
    double lambda;
    double alpha;
    double gamma;
    double min_child_weight;
    uint64_t seed;
} dyn_tree_opts_t;

static void dyn_tree_free(dyn_tree_t* t)
{
    free(t->node);
    free(t->improve);
    free(t->nsamp);
    free(t->proba);
    t->node = NULL;
    t->improve = t->nsamp = t->proba = NULL;
    t->n_nodes = t->cap = t->n_classes = 0;
}

static size_t dyn_tree_push(dyn_tree_t* t)
{
    if (t->n_nodes == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 32, k = t->n_classes;
        dyn_tnode_t* p = (dyn_tnode_t*)realloc(t->node, cap * sizeof(*p));
        double *im, *ns, *pr = NULL;
        if (!p)
            return SIZE_MAX;
        t->node = p;
        im = (double*)realloc(t->improve, cap * sizeof(double));
        if (!im)
            return SIZE_MAX;
        t->improve = im;
        ns = (double*)realloc(t->nsamp, cap * sizeof(double));
        if (!ns)
            return SIZE_MAX;
        t->nsamp = ns;
        if (k) {
            pr = (double*)realloc(t->proba, cap * k * sizeof(double));
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

static void dyn_tree_set_proba(dyn_tree_t* t, size_t self, const double* cnt,
    size_t n)
{
    size_t i, k = t->n_classes;
    double tot = 0.0;
    if (!k || n == 0)
        return;
    for (i = 0; i < k; i++)
        tot += cnt[i];
    if (tot <= 0.0)
        return;
    for (i = 0; i < k; i++)
        t->proba[self * k + i] = cnt[i] / tot;
}

typedef struct {
    double key;
    uint32_t row;
    uint32_t pad;
} dyn_tree_kv_t;

static int dyn_ml_kv_cmp(const void* a, const void* b)
{
    const dyn_tree_kv_t *x = (const dyn_tree_kv_t*)a, *y = (const dyn_tree_kv_t*)b;
    if (x->key != y->key)
        return (x->key < y->key) ? -1 : 1;
    return (x->row > y->row) ? 1 : (x->row < y->row) ? -1
                                                     : 0;
}

static void dyn_ml_kv_hsort(dyn_tree_kv_t* a, size_t n)
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
            {
                dyn_tree_kv_t t = a[root];
                a[root] = a[c];
                a[c] = t;
            }
            root = c;
        }
    }
    for (i = n; i > 1; i--) {
        size_t root = 0, end = i - 1;
        {
            dyn_tree_kv_t t = a[0];
            a[0] = a[end];
            a[end] = t;
        }
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= end)
                break;
            c = (l + 1 < end && a[l + 1].key > a[l].key) ? l + 1 : l;
            if (a[root].key >= a[c].key)
                break;
            {
                dyn_tree_kv_t t = a[root];
                a[root] = a[c];
                a[c] = t;
            }
            root = c;
        }
    }
}

static void dyn_ml_kv_sort_r(dyn_tree_kv_t* a, size_t n, size_t depth,
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
            while (a[lo].key < pivot)
                lo++;
            while (a[hi].key > pivot) {
                if (hi == 0)
                    break;
                hi--;
            }
            if (lo <= hi) {
                dyn_tree_kv_t t = a[lo];
                a[lo] = a[hi];
                a[hi] = t;
                lo++;
                if (hi == 0)
                    break;
                hi--;
            }
        }
        if (hi + 1 > 1)
            dyn_ml_kv_sort_r(a, hi + 1, depth + 1, limit);
        if (lo < n)
            dyn_ml_kv_sort_r(a + lo, n - lo, depth + 1, limit);
    }
}

static void dyn_tree_sort_kv(dyn_tree_kv_t* a, size_t n)
{
    size_t limit = 0, t;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_kv_sort_r(a, n, 0, 2 * limit + 1);
}

static void dyn_tree_sort_idx(const double* X, size_t cols, int32_t f,
    uint32_t* idx, size_t n, dyn_tree_kv_t* kv)
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

#define DYN_HIST_MAX_BINS 255u

typedef struct {
    uint8_t* code;
    double *vmin, *vmax;
    uint32_t* nbin;
    size_t max_bins, rows, cols;
    int exact;
} dyn_hist_t;

static void dyn_hist_free(dyn_hist_t* h)
{
    free(h->code);
    free(h->vmin);
    free(h->vmax);
    free(h->nbin);
    memset(h, 0, sizeof(*h));
}

static double dyn_hist_mid(double lo, double hi)
{
    double t = lo + (hi - lo) * 0.5;
    return (t >= hi) ? lo : t;
}

static int dyn_hist_build(dyn_hist_t* h, const double* X, size_t rows,
    size_t cols, size_t max_bins, dyn_tree_kv_t* kv,
    double* vals, const double* w)
{
    size_t f, i, b, m, ndist, nb;

    memset(h, 0, sizeof(*h));
    if (max_bins < 2)
        max_bins = 2;
    if (max_bins > DYN_HIST_MAX_BINS)
        max_bins = DYN_HIST_MAX_BINS;
    if (rows == 0 || cols == 0 || cols > SIZE_MAX / rows)
        return -1;
    if (cols > (SIZE_MAX / sizeof(double)) / max_bins || cols > SIZE_MAX / sizeof(uint32_t))
        return -1;
    h->rows = rows;
    h->cols = cols;
    h->max_bins = max_bins;
    h->exact = 1;
    h->code = (uint8_t*)malloc(rows * cols);
    h->vmin = (double*)malloc(cols * max_bins * sizeof(double));
    h->vmax = (double*)malloc(cols * max_bins * sizeof(double));
    h->nbin = (uint32_t*)malloc(cols * sizeof(uint32_t));
    if (!h->code || !h->vmin || !h->vmax || !h->nbin) {
        dyn_hist_free(h);
        return -1;
    }

    for (f = 0; f < cols; f++) {
        double *lo_v = h->vmin + f * max_bins, *hi_v = h->vmax + f * max_bins;
        m = 0;
        for (i = 0; i < rows; i++) {
            double v = X[i * cols + f];
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
            nb = 1;
            lo_v[0] = hi_v[0] = 0.0;
        } else if (ndist <= max_bins) {
            nb = ndist;
            for (b = 0; b < ndist; b++)
                lo_v[b] = hi_v[b] = vals[b];
        } else {
            size_t start = 0;
            h->exact = 0;
            nb = max_bins;
            for (b = 0; b < nb; b++) {
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

static double dyn_newton_score(double G, double H, double lambda, double alpha)
{
    double g = fabs(G) - alpha, d = H + lambda;
    if (g < 0.0)
        g = 0.0;
    return (d > 0.0) ? g * g / d : 0.0;
}

typedef struct {
    const double* X;
    const double* y;
    size_t cols;
    const double* classes;
    size_t n_classes;
    double* cnt_left;
    double* cnt_right;
    double* cnt_tot;
    const uint8_t* cls_of;
    const double* w;
    uint32_t* feat;
    size_t n_cand;
    uint32_t* scratch;
    dyn_tree_kv_t* kv;
    uint32_t* best_order;
    const dyn_hist_t* hist;
    double* hbuf;
    uint32_t* hnext;
    size_t hstride;
    const double* grad;
    const double* hess;
    dyn_tree_opts_t opt;
    uint64_t rng;
    JSContext* ctx;
    int interrupted;
} dyn_tree_ctx_t;

static size_t dyn_tree_class_of(const dyn_tree_ctx_t* c, uint32_t r)
{
    return c->cls_of ? c->cls_of[r] : 0;
}

static double dyn_tree_leaf_value(dyn_tree_ctx_t* c, const uint32_t* idx,
    size_t n)
{
    size_t i;
    if (c->opt.newton) {
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
                best = i;
        return c->classes[best];
    }
}

static double dyn_tree_gini(const double* cnt, size_t k, double tot)
{
    double s = 0.0;
    size_t i;
    if (tot <= 0.0)
        return 0.0;
    for (i = 0; i < k; i++)
        s += cnt[i] * cnt[i];
    return tot - s / tot;
}

static void dyn_split_hist(dyn_tree_ctx_t* c, const uint32_t* idx, size_t n,
    size_t nf, int32_t* out_f, double* out_thr,
    double* out_gain, uint32_t* out_bin, int* out_mleft)
{
    const dyn_hist_t* H = c->hist;
    const uint8_t* code = H->code;
    size_t stride = c->hstride, ncls = c->n_classes;
    size_t k, b, i, j, msl = c->opt.min_samples_leaf;
    double lambda = c->opt.lambda, alpha = c->opt.alpha, gam = c->opt.gamma;
    double mcw = c->opt.min_child_weight;
    int cls = c->opt.classifier, newton = c->opt.newton;
    size_t cslot = c->w ? stride - 1 : 0;

    for (k = 0; k < nf; k++) {
        int32_t f = (int32_t)c->feat[k];
        const uint8_t* cf = code + (size_t)f * H->rows;
        size_t nb = H->nbin[f], miss = nb;
        const double* lo_v = H->vmin + (size_t)f * H->max_bins;
        const double* hi_v = H->vmax + (size_t)f * H->max_bins;
        double* hb = c->hbuf;
        double tot_c = 0.0, tot_1 = 0.0, tot_2 = 0.0, parent = 0.0;
        double tot_n = 0.0;
        size_t lo_b = nb, hi_b = 0;
        int dir;

        if (newton) {
            const double *g = c->grad, *hh = c->hess;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += 1.0;
                s[1] += g[r];
                s[2] += hh[r];
            }
        } else if (c->w && cls) {
            const double* tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += tw[r];
                s[1 + dyn_tree_class_of(c, r)] += tw[r];
                s[cslot] += 1.0;
            }
        } else if (c->w) {
            const double *ty = c->y, *tw = c->w;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
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
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                s[0] += 1.0;
                s[1 + dyn_tree_class_of(c, r)] += 1.0;
            }
        } else {
            const double* ty = c->y;
            for (i = 0; i < n; i++) {
                uint32_t r = idx[i];
                size_t bc = cf[r];
                double* s = hb + bc * stride;
                if (bc < lo_b)
                    lo_b = bc;
                if (bc > hi_b)
                    hi_b = bc;
                double v = ty[r];
                s[0] += 1.0;
                s[1] += v;
                s[2] += v * v;
            }
        }

        if (cls && !newton) {
            memset(c->cnt_tot, 0, ncls * sizeof(double));
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double* s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            {
                const double* s = hb + nb * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                for (j = 0; j < ncls; j++)
                    c->cnt_tot[j] += s[1 + j];
            }
            parent = dyn_tree_gini(c->cnt_tot, ncls, tot_c);
        } else {
            for (b = lo_b; b < nb && lo_b <= hi_b; b++) {
                const double* s = hb + b * stride;
                tot_c += s[0];
                tot_n += s[cslot];
                tot_1 += s[1];
                tot_2 += s[2];
            }
            {
                const double* s = hb + nb * stride;
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

        if (lo_b <= hi_b) {
            uint32_t nxt = (uint32_t)nb;
            c->hnext[hi_b + 1] = nxt;
            for (b = hi_b + 1; b-- > lo_b;) {
                if (hb[b * stride] > 0.0)
                    nxt = (uint32_t)b;
                c->hnext[b] = nxt;
            }
        }

        for (dir = 0; dir < 2; dir++) {
            double cl = 0.0, s1 = 0.0, s2 = 0.0, nl_cnt = 0.0;
            const double* ms = hb + miss * stride;
            size_t prev_nz = nb;
            if (dir == 1 && ms[0] == 0.0)
                break;
            if (cls && !newton)
                memset(c->cnt_left, 0, ncls * sizeof(double));
            if (dir == 1) {
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
            for (b = 0; b <= (lo_b <= hi_b ? hi_b + 1 : 0); b = (b ? b + 1 : lo_b + 1)) {
                double gain, cr, thr;
                size_t br;
                if (b > 0) {
                    const double* s = hb + (b - 1) * stride;
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
                if (nl_cnt < (double)msl || tot_n - nl_cnt < (double)msl)
                    continue;
                if (cl <= 0.0 || cr <= 0.0)
                    continue;
                if (newton) {
                    double gr = tot_1 - s1, hr = tot_2 - s2;
                    if (s2 < mcw || hr < mcw)
                        continue;
                    gain = 0.5 * (dyn_newton_score(s1, s2, lambda, alpha) + dyn_newton_score(gr, hr, lambda, alpha) - parent) - gam;
                } else if (cls) {
                    for (j = 0; j < ncls; j++)
                        c->cnt_right[j] = c->cnt_tot[j] - c->cnt_left[j];
                    gain = parent - (dyn_tree_gini(c->cnt_left, ncls, cl) + dyn_tree_gini(c->cnt_right, ncls, cr)) - gam;
                } else {
                    double r1 = tot_1 - s1, r2 = tot_2 - s2;
                    double sse_l = s2 - s1 * s1 / cl;
                    double sse_r = r2 - r1 * r1 / cr;
                    gain = parent - (sse_l + sse_r) - gam;
                }
                if (gain > *out_gain) {
                    br = (b >= lo_b && b <= hi_b + 1) ? c->hnext[b] : (uint32_t)lo_b;
                    if (prev_nz == nb)
                        thr = -DYN_INFINITY;
                    else if (br < nb)
                        thr = dyn_hist_mid(hi_v[prev_nz], lo_v[br]);
                    else
                        thr = hi_v[prev_nz];
                    *out_gain = gain;
                    *out_f = f;
                    *out_thr = thr;
                    *out_bin = (uint32_t)b;
                    *out_mleft = dir;
                }
            }
        }
        if (lo_b <= hi_b)
            memset(hb + lo_b * stride, 0, (hi_b - lo_b + 1) * stride * sizeof(double));
        memset(hb + nb * stride, 0, stride * sizeof(double));
    }
}

static size_t dyn_tree_depth_limit(size_t requested)
{
    return (requested && requested < DYN_TREE_MAX_DEPTH)
        ? requested
        : DYN_TREE_MAX_DEPTH;
}

static size_t dyn_tree_build(dyn_tree_ctx_t* c, dyn_tree_t* t, uint32_t* idx,
    size_t n, size_t depth)
{
    size_t self, i, k, nf, nl, left, right, ncand;
    int32_t best_f = -1;
    double best_thr = 0.0, best_gain = 0.0;
    uint32_t best_bin = 0;
    int best_mleft = 0;
    int pure = 1;

    if (c->interrupted || JS_CheckInterrupt(c->ctx)) {
        c->interrupted = 1;
        return SIZE_MAX;
    }
    self = dyn_tree_push(t);
    if (self == SIZE_MAX)
        return SIZE_MAX;
    t->node[self].left = t->node[self].right = DYN_TREE_LEAF;
    t->node[self].feature = -1;
    if (c->w) {
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
    if (pure || n < c->opt.min_samples_split || n < 2 * c->opt.min_samples_leaf || depth >= dyn_tree_depth_limit(c->opt.max_depth)) {
        t->node[self].value = dyn_tree_leaf_value(c, idx, n);
        dyn_tree_set_proba(t, self, c->cnt_left, n);
        return self;
    }

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

        {
            const double* TX = c->X;
            const double* ty = c->y;
            const uint32_t* scr = c->scratch;
            size_t cols = c->cols, msl = c->opt.min_samples_leaf;
            double xprev = TX[(size_t)scr[0] * cols + f];

            if (c->w) {
                const double* tw = c->w;
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
                            gain = parent - (dyn_tree_gini(cl_l, ncls, wt_l) + dyn_tree_gini(cl_r, ncls, wt_r));
                        } else {
                            double sum_r = sum_all - sum_l, sq_r = sq_all - sq_l;
                            gain = parent - ((sq_l - sum_l * sum_l / wt_l) + (sq_r - sum_r * sum_r / wt_r));
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
                            best_thr = xprev + (xcur - xprev) * 0.5;
                            if (best_thr >= xcur)
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
                        double gain = parent - (dyn_tree_gini(cl_l, ncls, (double)i) + dyn_tree_gini(cl_r, ncls, (double)(n - i)));
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
        const dyn_hist_t* H = c->hist;
        const uint8_t* cf = H->code + (size_t)best_f * H->rows;
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

static double dyn_tree_predict_row(const dyn_tree_t* t, const double* x)
{
    uint32_t at = 0;
    while (t->node[at].feature >= 0) {
        at = (x[t->node[at].feature] <= t->node[at].threshold)
            ? t->node[at].left
            : t->node[at].right;
    }
    return t->node[at].value;
}

static double dyn_tree_predict_row_missing(const dyn_tree_t* t, const double* x)
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

typedef struct {
    uint32_t* scratch;
    uint32_t* feat;
    double *cnt_l, *cnt_r, *cnt_tot;
    dyn_tree_kv_t* kv;
    uint32_t* best_order;
    const dyn_hist_t* hist;
    double* hbuf;
    uint32_t* hnext;
    size_t hstride;
    const double *grad, *hess;
    const double* w;
    const uint8_t* cls_of;
    size_t n_cand;
} dyn_tree_work_t;

static int dyn_tree_fit_one(JSContext* ctx, dyn_tree_t* t, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* classes, size_t n_classes,
    const dyn_tree_opts_t* opt, uint64_t* rng,
    uint32_t* idx, size_t n, const dyn_tree_work_t* w)
{
    dyn_tree_ctx_t c;
    size_t i;

    (void)rows;
    c.ctx = ctx;
    c.interrupted = 0;
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
    if (!w->n_cand)
        for (i = 0; i < cols; i++)
            c.feat[i] = (uint32_t)i;
    t->n_classes = opt->classifier ? n_classes : 0;
    if (dyn_tree_build(&c, t, idx, n, 0) == SIZE_MAX) {
        if (!c.interrupted)
            JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    *rng = c.rng;
    return 0;
}

typedef struct {
    int fitted;
    int classifier;
    int boosting;
    size_t n_trees;
    size_t n_rounds;
    size_t n_features;
    size_t n_classes;
    double* classes;
    double* raw_base;
    size_t n_out;
    double lr;
    double subsample;
    double colsample;
    size_t early_stop;
    double val_frac;
    size_t best_rounds;
    dyn_tree_t* trees;
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

static void dyn_forest_dispose(void* native)
{
    dyn_forest_t* m = (dyn_forest_t*)native;
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

static int dyn_opt_size(JSContext* ctx, JSValueConst obj, const char* name,
    size_t* out, size_t minimum)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int64_t iv;
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
    if (!isfinite(dv)) {
        JS_ThrowRangeError(ctx, "dyna:ml %s must be a finite number", name);
        return -1;
    }
    if (!(dv >= -9.2233720368547758e18 && dv < 9.2233720368547758e18)) {
        JS_ThrowRangeError(ctx, "dyna:ml %s is out of range", name);
        return -1;
    }
    iv = (int64_t)dv;
    if (iv < (int64_t)minimum) {
        JS_ThrowRangeError(ctx, "dyna:ml %s must be at least %u", name,
            (unsigned)minimum);
        return -1;
    }
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t)iv > (uint64_t)SIZE_MAX) {
        JS_ThrowRangeError(ctx, "dyna:ml %s is out of range", name);
        return -1;
    }
#endif
    *out = (size_t)iv;
    return 0;
}

static int dyn_opt_double(JSContext* ctx, JSValueConst obj, const char* name,
    double* out, double lo, double hi)
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
        JS_ThrowRangeError(ctx, "dyna:ml %s must be in [%g, %g]", name, lo, hi);
        return -1;
    }
    *out = dv;
    return 0;
}

static JSValue dyn_forest_new(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv,
    JSClassID class_id, int classifier, int boosting,
    size_t default_trees, size_t default_depth,
    int newton)
{
    dyn_forest_t* m;
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
        opt.max_bins = DYN_HIST_MAX_BINS;
        opt.lambda = 1.0;
        opt.alpha = 0.0;
        opt.gamma = 0.0;
        opt.min_child_weight = 1.0;
    }

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        const char* ks[16];
        int nk = 0, kt;
        for (kt = 0; kt < 9; kt++)
            ks[nk++] = ml_tree_keys[kt];
        if (newton) {
            for (kt = 0; kt < 7; kt++)
                ks[nk++] = ml_tree_xgb_extra_keys[kt];
        } else {
            ks[nk++] = "validationFraction";
            ks[nk++] = "earlyStoppingRounds";
        }
        if (dyn_opts_strict(ctx, o, ks, nk))
            return JS_EXCEPTION;
        seed_holder = (size_t)seed;
        if (newton && (dyn_opt_double(ctx, o, "lambda", &opt.lambda, 0.0, 1e12) || dyn_opt_double(ctx, o, "alpha", &opt.alpha, 0.0, 1e12) || dyn_opt_double(ctx, o, "gamma", &opt.gamma, 0.0, 1e12) || dyn_opt_double(ctx, o, "minChildWeight", &opt.min_child_weight, 0.0, 1e12) || dyn_opt_double(ctx, o, "colsampleByTree", &colsample, 1e-6, 1.0) || dyn_opt_double(ctx, o, "validationFraction", &val_frac, 0.0, 0.5) || dyn_opt_size(ctx, o, "earlyStoppingRounds", &early_stop, 0)))
            return JS_EXCEPTION;
        if (!newton) {
            JSValue v = JS_GetPropertyStr(ctx, o, "earlyStoppingRounds");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml earlyStoppingRounds is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, o, "validationFraction");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml validationFraction is only supported by the XGB models");
            }
            JS_FreeValue(ctx, v);
        }
        if (dyn_opt_size(ctx, o, "nEstimators", &n_trees, 1) || dyn_opt_size(ctx, o, "maxDepth", &opt.max_depth, 0) || dyn_opt_size(ctx, o, "minSamplesSplit", &opt.min_samples_split, 2) || dyn_opt_size(ctx, o, "minSamplesLeaf", &opt.min_samples_leaf, 1) || dyn_opt_size(ctx, o, "maxFeatures", &opt.max_features, 0) || dyn_opt_size(ctx, o, "maxBins", &opt.max_bins, 0) || dyn_opt_size(ctx, o, "seed", &seed_holder, 0) || dyn_opt_double(ctx, o, "learningRate", &lr, 1e-12, 1e12) || dyn_opt_double(ctx, o, "subsample", &subsample, 1e-6, 1.0))
            return JS_EXCEPTION;
        seed = (uint64_t)seed_holder;
        if (opt.max_bins && (opt.max_bins < 2 || opt.max_bins > DYN_HIST_MAX_BINS))
            return JS_ThrowRangeError(ctx, "dyna:ml maxBins must be 0 (exact splits) "
                                           "or between 2 and %u",
                (unsigned)DYN_HIST_MAX_BINS);
        if (newton && opt.max_bins == 0)
            return JS_ThrowRangeError(ctx, "dyna:ml maxBins 0 (exact splits) is not "
                                           "supported with the Newton objective");
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "dyna:ml expected an options object");
    }
    if (!boosting)
        n_trees = (default_trees == 1) ? 1 : n_trees;
    if (n_trees > DYN_ML_MAX_TREES)
        return JS_ThrowRangeError(ctx, "dyna:ml nEstimators must be at most %u",
            (unsigned)DYN_ML_MAX_TREES);
    if (opt.max_depth > DYN_TREE_MAX_DEPTH)
        return JS_ThrowRangeError(ctx,
            "dyna:ml maxDepth must be at most %u; a deeper tree is grown by "
            "recursion and would exhaust the C stack",
            (unsigned)DYN_TREE_MAX_DEPTH);

    m = (dyn_forest_t*)calloc(1, sizeof(*m));
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
    return dyn_res_wrap(ctx, new_target, class_id, m, dyn_forest_dispose);
}

static uint32_t dyn_tree_leaf_index(const dyn_tree_t* t, const double* x,
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

static uint32_t dyn_tree_leaf_index_missing(const dyn_tree_t* t, const double* x)
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

static void dyn_boost_leaf_newton(dyn_tree_t* t, const double* X, size_t cols,
    const double* grad, const double* hess,
    const double* w, const uint32_t* rows_in,
    size_t n, double scale, double* num,
    double* den)
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

static void dyn_boost_init_base(int classifier, size_t n_out, const double* y,
    size_t rows, const double* classes,
    const double* w, double* raw_base, double* pred)
{
    size_t i, k;
    double wtot = 0.0;
    for (i = 0; i < rows; i++)
        wtot += w ? w[i] : 1.0;
    if (wtot <= 0.0)
        wtot = 1.0;

    if (!classifier) {
        double s = 0.0;
        for (i = 0; i < rows; i++)
            s += w ? w[i] * y[i] : y[i];
        raw_base[0] = s / wtot;
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else if (n_out == 1) {
        double pos = 0.0, p;
        for (i = 0; i < rows; i++)
            if (y[i] == classes[1])
                pos += w ? w[i] : 1.0;
        p = pos / wtot;
        if (p < 1e-9)
            p = 1e-9;
        if (p > 1.0 - 1e-9)
            p = 1.0 - 1e-9;
        raw_base[0] = log(p / (1.0 - p));
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else {
        for (k = 0; k < n_out; k++) {
            double cnt = 0.0, p;
            for (i = 0; i < rows; i++)
                if (y[i] == classes[k])
                    cnt += w ? w[i] : 1.0;
            p = cnt / wtot;
            if (p < 1e-9)
                p = 1e-9;
            raw_base[k] = log(p);
        }
        for (i = 0; i < rows; i++)
            for (k = 0; k < n_out; k++)
                pred[i * n_out + k] = raw_base[k];
    }
}

static void dyn_xgb_grad_hess(int classifier, size_t n_out, size_t k,
    const double* pred, const double* y,
    const double* classes, const double* w,
    size_t rows, double* g, double* h)
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
            const double* f = pred + i * n_out;
            double mx = f[0], s = 0.0, ek = 0.0, p;
            for (j = 1; j < n_out; j++)
                if (f[j] > mx)
                    mx = f[j];
            for (j = 0; j < n_out; j++) {
                double e = exp(f[j] - mx);
                if (j == k)
                    ek = e;
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

static double dyn_xgb_val_loss(int classifier, size_t n_out, const double* pred,
    const double* y, const double* classes,
    const uint8_t* isval, size_t rows)
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
            if (p < 1e-15)
                p = 1e-15;
            if (p > 1.0 - 1e-15)
                p = 1.0 - 1e-15;
            acc += t ? -log(p) : -log(1.0 - p);
        } else {
            const double* f = pred + i * n_out;
            double mx = f[0], s = 0.0, own = 0.0;
            for (k = 1; k < n_out; k++)
                if (f[k] > mx)
                    mx = f[k];
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

static int dyn_forest_learn(JSContext* ctx, dyn_forest_t* m, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* w)
{
    dyn_tree_t* trees = NULL;
    uint32_t *idx = NULL, *scratch = NULL, *feat = NULL, *train = NULL;
    uint8_t* isval = NULL;
    dyn_tree_kv_t* kv = NULL;
    uint32_t* best_order = NULL;
    double *cnt_l = NULL, *cnt_r = NULL, *cnt_tot = NULL, *classes = NULL;
    double *resid = NULL, *pred = NULL, *hess = NULL, *raw_base = NULL;
    double *lnum = NULL, *lden = NULL, *hbuf = NULL, *binvals = NULL;
    size_t lnum_cap = 0;
    uint32_t* hnext = NULL;
    uint8_t* cls_of = NULL;
    size_t n_classes = 1, i, ti, use_n, n_out = 1, n_alloc, rounds, n_keep;
    uint64_t rng = m->seed;
    dyn_tree_opts_t opt = m->opt;
    dyn_hist_t hist = { 0 };
    dyn_tree_work_t work;
    int have_hist = 0, rc = -1;

    if (m->classifier) {
        n_classes = dyn_ml_classes(ctx, y, rows, &classes);
        if (n_classes == 0)
            return -1;
        if (n_classes > 256) {
            free(classes);
            JS_ThrowRangeError(ctx,
                "ml.fit: tree models support at most 256 classes, found %u",
                (unsigned)n_classes);
            return -1;
        }
    }
    rounds = m->n_rounds;
    if (m->boosting && m->classifier) {
        if (n_classes < 2) {
            free(classes);
            JS_ThrowTypeError(ctx, "ml.fit: %s needs at least two classes in y, "
                                   "found %u",
                opt.newton ? "XGBClassifier"
                           : "GradientBoostingClassifier",
                (unsigned)n_classes);
            return -1;
        }
        n_out = (n_classes == 2) ? 1 : n_classes;
    }
    if (m->boosting && n_out && rounds > SIZE_MAX / n_out) {
        JS_ThrowRangeError(ctx, "ml.fit: allocation size overflow");
        goto done;
    }
    n_alloc = m->boosting ? rounds * n_out : rounds;
    if (n_alloc > SIZE_MAX / sizeof(dyn_tree_t) || rows > SIZE_MAX / sizeof(dyn_tree_kv_t) || rows > SIZE_MAX / sizeof(uint32_t) || cols > SIZE_MAX / sizeof(uint32_t) || n_classes > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "ml.fit: allocation size overflow");
        goto done;
    }
    n_keep = n_alloc;
    trees = (dyn_tree_t*)calloc(n_alloc ? n_alloc : 1, sizeof(*trees));
    idx = (uint32_t*)malloc(rows * sizeof(uint32_t));
    scratch = (uint32_t*)malloc(rows * sizeof(uint32_t));
    kv = (dyn_tree_kv_t*)malloc(rows * sizeof(dyn_tree_kv_t));
    best_order = (uint32_t*)malloc(rows * sizeof(uint32_t));
    feat = (uint32_t*)malloc(cols * sizeof(uint32_t));
    cnt_l = (double*)malloc(n_classes * sizeof(double));
    cnt_r = (double*)malloc(n_classes * sizeof(double));
    cnt_tot = (double*)malloc(n_classes * sizeof(double));
    if (opt.classifier && classes) {
        cls_of = (uint8_t*)malloc(rows ? rows : 1);
        if (cls_of) {
            for (i = 0; i < rows; i++) {
                size_t kk;
                cls_of[i] = 0;
                for (kk = 0; kk < n_classes; kk++)
                    if (classes[kk] == y[i]) {
                        cls_of[i] = (uint8_t)kk;
                        break;
                    }
            }
        }
    }
    if (m->boosting) {
        if (rows > (SIZE_MAX / sizeof(double)) / n_out) {
            JS_ThrowRangeError(ctx, "ml.fit: data too large for boosting");
            goto done;
        }
        resid = (double*)malloc(rows * sizeof(double));
        pred = (double*)malloc(rows * n_out * sizeof(double));
        raw_base = (double*)calloc(n_out, sizeof(double));
        if (m->classifier || opt.newton)
            hess = (double*)malloc(rows * sizeof(double));
    }
    if (opt.newton) {
        isval = (uint8_t*)calloc(rows, sizeof(uint8_t));
        train = (uint32_t*)malloc(rows * sizeof(uint32_t));
    }
    if (!trees || !idx || !scratch || !kv || !best_order || !feat || !cnt_l || !cnt_r || !cnt_tot || (opt.classifier && classes && !cls_of) || (opt.newton && (!isval || !train)) || (m->boosting && (!resid || !pred || !raw_base)) || (m->boosting && (m->classifier || opt.newton) && !hess)) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memset(&work, 0, sizeof(work));
    work.w = opt.newton ? NULL : w;
    work.cls_of = cls_of;
    work.scratch = scratch;
    work.feat = feat;
    work.cnt_l = cnt_l;
    work.cnt_r = cnt_r;
    work.cnt_tot = cnt_tot;
    work.kv = kv;
    work.best_order = best_order;
    if (opt.max_bins) {
        size_t payload = (opt.classifier && n_classes > 2) ? n_classes : 2;
        size_t stride = 1 + payload + ((w && !opt.newton) ? 1 : 0);
        binvals = (double*)malloc(rows * sizeof(double));
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
        hbuf = (double*)calloc((hist.max_bins + 1) * stride, sizeof(double));
        hnext = (uint32_t*)malloc((hist.max_bins + 2) * sizeof(uint32_t));
        if (!hbuf || !hnext) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        work.hist = &hist;
        work.hbuf = hbuf;
        work.hnext = hnext;
        work.hstride = stride;
    }
    if (opt.max_features == 0 && m->n_rounds > 1 && !m->boosting) {
        double r = m->classifier ? sqrt((double)cols) : (double)cols / 3.0;
        size_t v = (size_t)(r + 0.5);
        opt.max_features = v ? v : 1;
    }

    if (opt.newton) {
        dyn_tree_opts_t topt = opt;
        size_t r, k, ntrain, nval = 0, kept = rounds, since = 0, best_round = 0;
        double best_loss = 0.0;
        int have_best = 0;

        topt.classifier = 0;
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
            raw_base, pred);

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
                    size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(ntrain - i));
                    uint32_t tmp = idx[i];
                    idx[i] = idx[j];
                    idx[j] = tmp;
                }
            }
            for (k = 0; k < n_out; k++) {
                dyn_tree_t* tr = &trees[r * n_out + k];
                size_t ncand = (size_t)((double)cols * m->colsample + 0.5);
                if (ncand < 1)
                    ncand = 1;
                if (ncand > cols)
                    ncand = cols;
                for (i = 0; i < cols; i++)
                    feat[i] = (uint32_t)i;
                if (ncand < cols) {
                    for (i = 0; i < ncand; i++) {
                        size_t j = i + (size_t)(dyn_splitmix64(&rng) % (uint64_t)(cols - i));
                        uint32_t tmp = feat[i];
                        feat[i] = feat[j];
                        feat[j] = tmp;
                    }
                }
                work.n_cand = ncand;
                dyn_xgb_grad_hess(m->classifier, n_out, k, pred, y, classes, w,
                    rows, resid, hess);
                work.grad = resid;
                work.hess = hess;
                if (dyn_tree_fit_one(ctx, tr, X, resid, rows, cols, NULL, 1,
                        &topt, &rng, idx, use_n, &work))
                    goto done;
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr * dyn_tree_predict_row_missing(tr, X + i * cols);
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
        if (have_best)
            kept = best_round;
        n_keep = kept * n_out;
        m->best_rounds = kept;
    } else if (m->boosting) {
        dyn_tree_opts_t topt = opt;
        size_t r, k;
        topt.classifier = 0;
        dyn_boost_init_base(m->classifier, n_out, y, rows, classes, w,
            raw_base, pred);
        use_n = (size_t)((double)rows * m->subsample + 0.5);
        if (use_n < 1)
            use_n = 1;
        if (use_n > rows)
            use_n = rows;
        for (r = 0; r < rounds; r++) {
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
                dyn_tree_t* tr = &trees[r * n_out + k];
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
                    for (i = 0; i < rows; i++) {
                        const double* f = pred + i * n_out;
                        double mx = f[0], s = 0.0, p;
                        size_t j;
                        for (j = 1; j < n_out; j++)
                            if (f[j] > mx)
                                mx = f[j];
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
                    if (tr->n_nodes > lnum_cap) {
                        double *nn, *dd;
                        size_t want = lnum_cap ? lnum_cap : 16;
                        while (want < tr->n_nodes && want <= SIZE_MAX / 16)
                            want *= 2;
                        if (want < tr->n_nodes)
                            want = tr->n_nodes;
                        nn = (double*)realloc(lnum, want * sizeof(double));
                        if (!nn) {
                            JS_ThrowOutOfMemory(ctx);
                            goto done;
                        }
                        lnum = nn;
                        dd = (double*)realloc(lden, want * sizeof(double));
                        if (!dd) {
                            JS_ThrowOutOfMemory(ctx);
                            goto done;
                        }
                        lden = dd;
                        lnum_cap = want;
                    }
                    dyn_boost_leaf_newton(tr, X, cols, resid, hess, w, idx,
                        use_n, scale, lnum, lden);
                }
                for (i = 0; i < rows; i++)
                    pred[i * n_out + k] += m->lr * dyn_tree_predict_row(tr, X + i * cols);
            }
        }
    } else {
        for (ti = 0; ti < rounds; ti++) {
            if (rounds == 1) {
                for (i = 0; i < rows; i++)
                    idx[i] = (uint32_t)i;
            } else {
                uint64_t M = dyn_fastmod_M((uint64_t)rows);
                for (i = 0; i < rows; i++) {
                    uint64_t draw = dyn_splitmix64(&rng);
                    idx[i] = (uint32_t)(rows == 1 ? 0
                                                  : dyn_fastmod(draw, M, (uint64_t)rows));
                }
            }
            if (dyn_tree_fit_one(ctx, &trees[ti], X, y, rows, cols, classes,
                    n_classes, &opt, &rng, idx, rows, &work))
                goto done;
        }
    }
    if (m->trees) {
        for (i = 0; i < m->n_trees; i++)
            dyn_tree_free(&m->trees[i]);
        free(m->trees);
    }
    free(m->classes);
    free(m->raw_base);
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
        for (i = 0; i < n_alloc; i++)
            dyn_tree_free(&trees[i]);
        free(trees);
    }
    free(classes);
    free(idx);
    free(scratch);
    free(kv);
    free(best_order);
    free(feat);
    free(cnt_l);
    free(cnt_r);
    free(cnt_tot);
    free(resid);
    free(pred);
    free(hess);
    free(raw_base);
    free(lnum);
    free(lden);
    free(hbuf);
    free(binvals);
    free(hnext);
    free(isval);
    free(train);
    free(cls_of);
    if (have_hist)
        dyn_hist_free(&hist);
    return rc;
}

static JSValue dyn_forest_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, JSClassID class_id,
    int newton)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *w = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_ex(ctx, argv[0], argv[1], rows_arg, cols_arg,
            argc, argv, 2, &mx, &y, &w, newton))
        return JS_EXCEPTION;
    if (mx.cols > (size_t)INT32_MAX) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_ThrowRangeError(ctx, "ml.fit: too many columns for a tree model");
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    if (dyn_forest_learn(ctx, m, mx.data, y, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    free(w);
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

static double dyn_forest_raw_score(const dyn_forest_t* m, const double* x,
    size_t k)
{
    double acc = m->raw_base[k];
    size_t r, rounds = m->n_out ? m->n_trees / m->n_out : 0;
    if (m->opt.newton) {
        for (r = 0; r < rounds; r++)
            acc += m->lr * dyn_tree_predict_row_missing(&m->trees[r * m->n_out + k], x);
        return acc;
    }
    for (r = 0; r < rounds; r++)
        acc += m->lr * dyn_tree_predict_row(&m->trees[r * m->n_out + k], x);
    return acc;
}

static JSValue dyn_forest_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int into)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double *out = NULL, *votes = NULL;
    size_t rows, i, ti, c;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    if (rows > SIZE_MAX / sizeof(double)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: allocation size overflow");
    }
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * sizeof(double));
    }
    if (m->classifier && !votes)
        votes = (double*)calloc(m->n_classes, sizeof(double));
    if (!out || (m->classifier && !votes)) {
        if (!into)
            free(out);
        free(votes);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        free(votes);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
        if (m->boosting && m->classifier) {
            size_t k, best = 0;
            double bestf = 0.0;
            for (k = 0; k < m->n_out; k++) {
                double f = dyn_forest_raw_score(m, xi, k);
                if (k == 0 || f > bestf) {
                    bestf = f;
                    best = k;
                }
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
            size_t best = 0;
            memset(votes, 0, m->n_classes * sizeof(double));
            for (ti = 0; ti < m->n_trees; ti++) {
                const dyn_tree_t* t = &m->trees[ti];
                uint32_t leaf;
                if (t->n_classes != m->n_classes || !t->proba) {
                    double lab = dyn_tree_predict_row(t, xi);
                    for (c = 0; c < m->n_classes; c++)
                        if (m->classes[c] == lab) {
                            votes[c] += 1.0;
                            break;
                        }
                    continue;
                }
                leaf = dyn_tree_leaf_index(t, xi, rs.cols);
                for (c = 0; c < m->n_classes; c++)
                    votes[c] += t->proba[(size_t)leaf * m->n_classes + c];
            }
            for (c = 1; c < m->n_classes; c++)
                if (votes[c] > votes[best])
                    best = c;
            out[i] = m->classes[best];
        }
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    free(votes);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_predict_result(ctx, out, rows, as_f64);
    if (!into)
        free(out);
    return result;
}

static JSValue dyn_forest_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    return dyn_forest_predict_impl(ctx, this_val, argc, argv, class_id, 0);
}

static JSValue dyn_forest_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    return dyn_forest_predict_impl(ctx, this_val, argc, argv, class_id, 1);
}

static JSValue dyn_forest_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, ti, c, k;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (JS_GetClassID(argv[0]) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, argv[0]);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predictProba: predictProba before fit");
    }
    if (!m->classifier) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx,
            "ml.predictProba: predictProba is for classifiers; this model predicts a number");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predictProba: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    k = m->n_classes;
    out = (double*)calloc(rows * (k ? k : 1), sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
        if (m->boosting) {
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
            const dyn_tree_t* t = &m->trees[ti];
            uint32_t leaf;
            if (t->n_classes != k || !t->proba)
                continue;
            leaf = dyn_tree_leaf_index(t, xi, rs.cols);
            for (c = 0; c < k; c++)
                out[i * k + c] += t->proba[(size_t)leaf * k + c];
        }
        for (c = 0; c < k; c++)
            out[i * k + c] /= (double)m->n_trees;
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    result = dyn_ml_matrix_to_js(ctx, out, rows, k, 0);
    free(out);
    return result;
}

static JSValue dyn_forest_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    dyn_forest_t* m;
    dyn_matrix_t mx = { 0 };
    double* out;
    size_t rows, i, ti;
    JSValue result;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.apply: apply before fit");
    }
    if (mx.cols != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.apply: X has %llu features, model expects %llu",
            (unsigned long long)mx.cols,
            (unsigned long long)m->n_features);
    }
    rows = mx.rows;
    out = (double*)malloc(rows * (m->n_trees ? m->n_trees : 1) * sizeof(double));
    if (!out) {
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < rows; i++) {
        const double* xi = mx.data + i * mx.cols;
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

static JSValue dyn_forest_importances(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    double *imp, total = 0.0;
    size_t ti, i, f;
    JSValue result;

    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_ThrowInternalError(ctx, "ml.featureImportances: featureImportances before fit");
    imp = (double*)calloc(m->n_features ? m->n_features : 1, sizeof(double));
    if (!imp)
        return JS_ThrowOutOfMemory(ctx);
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t* t = &m->trees[ti];
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
    if (total > 0.0)
        for (f = 0; f < m->n_features; f++)
            imp[f] /= total;
    result = dyn_ml_doubles_to_js(ctx, imp, m->n_features);
    free(imp);
    return result;
}

static JSValue dyn_forest_depth(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    size_t ti, i, maxd = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    for (ti = 0; ti < m->n_trees; ti++) {
        const dyn_tree_t* t = &m->trees[ti];
        uint32_t* d = (uint32_t*)calloc(t->n_nodes ? t->n_nodes : 1,
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
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,      \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_fit(ctx, t, argc, argv, class_id_var, newton_model); \
    }                                                                          \
    static JSValue prefix##_predict(JSContext* ctx, JSValueConst t, int argc,  \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_predict(ctx, t, argc, argv, class_id_var);           \
    }                                                                          \
    static JSValue prefix##_depth(JSContext* ctx, JSValueConst t)              \
    {                                                                          \
        return dyn_forest_depth(ctx, t, class_id_var);                         \
    }                                                                          \
    static JSValue prefix##_proba(JSContext* ctx, JSValueConst t, int argc,    \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_predict_proba(ctx, t, argc, argv, class_id_var);     \
    }                                                                          \
    static JSValue prefix##_apply(JSContext* ctx, JSValueConst t, int argc,    \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_apply(ctx, t, argc, argv, class_id_var);             \
    }                                                                          \
    static JSValue prefix##_into(JSContext* ctx, JSValueConst t,               \
        int argc, JSValueConst* argv)                                          \
    {                                                                          \
        return dyn_forest_predict_into(ctx, t, argc, argv, class_id_var);      \
    }                                                                          \
    static JSValue prefix##_imp(JSContext* ctx, JSValueConst t)                \
    {                                                                          \
        return dyn_forest_importances(ctx, t, class_id_var);                   \
    }

#define DYN_FOREST_PROTO(prefix)                                  \
    static const JSCFunctionListEntry prefix##_proto[] = {        \
        JS_CFUNC_DEF("fit", 2, prefix##_fit),                     \
        JS_CFUNC_DEF("predict", 1, prefix##_predict),             \
        JS_CFUNC_DEF("predictInto", 2, prefix##_into),            \
        JS_CFUNC_DEF("predictProba", 1, prefix##_proba),          \
        JS_CFUNC_DEF("apply", 1, prefix##_apply),                 \
        JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL), \
        JS_CGETSET_DEF("depth", prefix##_depth, NULL),            \
    };

#define DYN_FOREST_METHODS(prefix, class_id_var, newton_model) \
    DYN_FOREST_FNS(prefix, class_id_var, newton_model)         \
    DYN_FOREST_PROTO(prefix)

DYN_FOREST_METHODS(dyn_dtc, dyn_dtc_class_id, 0)
DYN_FOREST_METHODS(dyn_dtr, dyn_dtr_class_id, 0)
DYN_FOREST_METHODS(dyn_rfc, dyn_rfc_class_id, 0)
DYN_FOREST_METHODS(dyn_rfr, dyn_rfr_class_id, 0)
DYN_FOREST_METHODS(dyn_gbr, dyn_gbr_class_id, 0)
DYN_FOREST_METHODS(dyn_gbc, dyn_gbc_class_id, 0)
DYN_FOREST_FNS(dyn_xgbr, dyn_xgbr_class_id, 1)
DYN_FOREST_FNS(dyn_xgbc, dyn_xgbc_class_id, 1)

static JSValue dyn_xgb_best(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->best_rounds : 0));
}

#define DYN_XGB_PROTO(prefix, class_id_var)                       \
    static JSValue prefix##_best(JSContext* ctx, JSValueConst t)  \
    {                                                             \
        return dyn_xgb_best(ctx, t, class_id_var);                \
    }                                                             \
    static const JSCFunctionListEntry prefix##_xproto[] = {       \
        JS_CFUNC_DEF("fit", 2, prefix##_fit),                     \
        JS_CFUNC_DEF("predict", 1, prefix##_predict),             \
        JS_CFUNC_DEF("predictInto", 2, prefix##_into),            \
        JS_CFUNC_DEF("predictProba", 1, prefix##_proba),          \
        JS_CFUNC_DEF("apply", 1, prefix##_apply),                 \
        JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL), \
        JS_CGETSET_DEF("depth", prefix##_depth, NULL),            \
        JS_CGETSET_DEF("bestRounds", prefix##_best, NULL),        \
    };

DYN_XGB_PROTO(dyn_xgbr, dyn_xgbr_class_id)
DYN_XGB_PROTO(dyn_xgbc, dyn_xgbc_class_id)

static JSValue dyn_dtc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_dtc_class_id, 1, 0, 1, 0, 0);
}
static JSValue dyn_dtr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_dtr_class_id, 0, 0, 1, 0, 0);
}
static JSValue dyn_rfc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_rfc_class_id, 1, 0, 100, 0, 0);
}
static JSValue dyn_rfr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_rfr_class_id, 0, 0, 100, 0, 0);
}
static JSValue dyn_gbr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_gbr_class_id, 0, 1, 100, 3, 0);
}
static JSValue dyn_gbc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_gbc_class_id, 1, 1, 100, 3, 0);
}
static JSValue dyn_xgbr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_xgbr_class_id, 0, 1, 100, 6, 1);
}
static JSValue dyn_xgbc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_xgbc_class_id, 1, 1, 100, 6, 1);
}

typedef struct {
    int fitted;
    int whiten;
    size_t n_features;
    size_t n_components;
    double* mean;
    double* comp;
    double* var;
    double total_var;
} dyn_pca_t;

static JSClassID dyn_pca_class_id;

static void dyn_pca_dispose(void* native)
{
    dyn_pca_t* m = (dyn_pca_t*)native;
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

static JSValue dyn_pca_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_pca_t* m;
    int64_t nc = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &nc, argv[0]))
            return JS_EXCEPTION;
        if (nc < 0)
            return JS_ThrowRangeError(ctx, "ml.PCA: nComponents must not be negative");
    }
    m = (dyn_pca_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->n_components = (size_t)nc;
    if (argc > 1) {
        m->whiten = JS_ToBool(ctx, argv[1]);
        if (m->whiten < 0)
            return JS_EXCEPTION;
    }
    return dyn_res_wrap(ctx, new_target, dyn_pca_class_id, m, dyn_pca_dispose);
}

#define DYN_JACOBI_SWEEPS 60

static void dyn_jacobi(double* A, double* V, size_t p)
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
        if (off <= 1e-300 || (sweep >= 2 && off >= prev_off))
            break;
        prev_off = off;
        for (i = 0; i < p; i++) {
            for (j = i + 1; j < p; j++) {
                double aij = A[i * p + j];
                double theta, t, c, s;
                if (fabs(aij) <= 1e-300)
                    continue;
                theta = (A[j * p + j] - A[i * p + i]) / (2.0 * aij);
                t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
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

static int dyn_pca_learn(JSContext* ctx, dyn_pca_t* m, const double* X,
    size_t rows, size_t cols)
{
    double *mean = NULL, *cov = NULL, *V = NULL, *row = NULL;
    double *comp = NULL, *var = NULL;
    size_t* order = NULL;
    size_t want, i, j, a, b;
    double denom, total = 0.0;

    if (rows < 2) {
        JS_ThrowRangeError(ctx, "ml.fit: PCA needs at least two rows");
        return -1;
    }
    want = m->n_components ? m->n_components : cols;
    if (want > cols) {
        JS_ThrowRangeError(ctx,
            "ml.fit: nComponents (%u) exceeds the number of features (%u)",
            (unsigned)want, (unsigned)cols);
        return -1;
    }
    if (cols > (SIZE_MAX / sizeof(double)) / cols) {
        JS_ThrowRangeError(ctx, "ml.fit: too many features for PCA");
        return -1;
    }
    mean = (double*)calloc(cols, sizeof(double));
    cov = (double*)calloc(cols * cols, sizeof(double));
    V = (double*)malloc(cols * cols * sizeof(double));
    row = (double*)malloc(cols * sizeof(double));
    comp = (double*)malloc(want * cols * sizeof(double));
    var = (double*)malloc(want * sizeof(double));
    order = (size_t*)malloc(cols * sizeof(size_t));
    if (!mean || !cov || !V || !row || !comp || !var || !order) {
        free(mean);
        free(cov);
        free(V);
        free(row);
        free(comp);
        free(var);
        free(order);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++)
        for (j = 0; j < cols; j++)
            mean[j] += X[i * cols + j];
    for (j = 0; j < cols; j++)
        mean[j] /= (double)rows;

    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
        for (j = 0; j < cols; j++)
            row[j] = xi[j] - mean[j];
        for (a = 0; a < cols; a++)
            dyn_ml_axpy(cov + a * cols + a, row[a], row + a, cols - a);
    }
    denom = (double)(rows - 1);
    for (a = 0; a < cols; a++)
        for (b = a; b < cols; b++) {
            double v = cov[a * cols + b] / denom;
            cov[a * cols + b] = v;
            cov[b * cols + a] = v;
        }
    for (a = 0; a < cols; a++)
        if (!isfinite(cov[a * cols + a])) {
            free(mean);
            free(cov);
            free(V);
            free(row);
            free(comp);
            free(var);
            free(order);
            JS_ThrowRangeError(ctx, "ml.fit: covariance overflowed; scale X before PCA");
            return -1;
        }

    if ((uint64_t)cols * cols * cols * DYN_JACOBI_SWEEPS > (uint64_t)1 << 36) {
        free(mean);
        free(cov);
        free(V);
        free(row);
        free(comp);
        free(var);
        free(order);
        JS_ThrowRangeError(ctx,
            "ml.fit: PCA on %zu columns exceeds the Jacobi work budget",
            cols);
        return -1;
    }

    dyn_jacobi(cov, V, cols);

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
        total += (ev > 0.0) ? ev : 0.0;
    }
    for (a = 0; a < want; a++) {
        size_t src = order[a];
        double ev = cov[src * cols + src];
        double maxabs = 0.0, scale = 1.0;
        size_t maxj = 0;
        var[a] = (ev > 0.0) ? ev : 0.0;
        for (j = 0; j < cols; j++) {
            double v = fabs(V[j * cols + src]);
            if (v > maxabs) {
                maxabs = v;
                maxj = j;
            }
        }
        if (V[maxj * cols + src] < 0.0)
            scale = -1.0;
        if (m->whiten) {
            double sd = sqrt(var[a]);
            scale /= (sd > 0.0) ? sd : 1.0;
        }
        for (j = 0; j < cols; j++)
            comp[a * cols + j] = V[j * cols + src] * scale;
    }
    free(cov);
    free(V);
    free(row);
    free(order);
    free(m->mean);
    free(m->comp);
    free(m->var);
    m->mean = mean;
    m->comp = comp;
    m->var = var;
    m->n_features = cols;
    m->n_components = want;
    m->total_var = total;
    m->fitted = 1;
    return 0;
}

static JSValue dyn_pca_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "PCA"))
        return JS_EXCEPTION;
    dyn_pca_t* m;
    dyn_matrix_t mx = { 0 };
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
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

enum { DYN_PCA_FWD,
    DYN_PCA_FIT_FWD,
    DYN_PCA_INV };

static JSValue dyn_pca_apply(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int mode, int into)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "PCA"))
        return JS_EXCEPTION;
    dyn_pca_t* m;
    dyn_matrix_t mx = { 0 };
    double *out = NULL, *row = NULL;
    size_t rows, i, j, a, in_cols, out_cols;
    int flat;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (mode == DYN_PCA_FIT_FWD) {
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
        return JS_ThrowInternalError(ctx, "ml.transform: transform before fit");
    }
    in_cols = (mode == DYN_PCA_INV) ? m->n_components : m->n_features;
    out_cols = (mode == DYN_PCA_INV) ? m->n_features : m->n_components;
    if (mx.cols != in_cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.transform: X has %u columns, expected %u",
            (unsigned)mx.cols, (unsigned)in_cols);
    }
    rows = mx.rows;
    flat = !mx.owned;
    if (out_cols == 0 || rows > (SIZE_MAX / sizeof(double)) / out_cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.transform: data too large for PCA transform");
    }
    if (into) {
        if (mode != DYN_PCA_FWD) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.transformInto: forward transform only; inverseTransform "
                "keeps its shape-in shape-out result");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows * out_cols, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc((rows * out_cols != 0 ? rows * out_cols : 1) * sizeof(double));
    }
    row = (double*)malloc((m->n_features ? m->n_features : 1) * sizeof(double));
    if (!out || !row) {
        if (!into)
            free(out);
        free(row);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (mode == DYN_PCA_INV) {
        for (i = 0; i < rows; i++) {
            const double* pi = mx.data + i * in_cols;
            double* oi = out + i * out_cols;
            for (j = 0; j < out_cols; j++)
                oi[j] = m->mean[j];
            for (a = 0; a < m->n_components; a++)
                dyn_ml_axpy(oi, pi[a], m->comp + a * m->n_features,
                    m->n_features);
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double* xi = mx.data + i * in_cols;
            double* oi = out + i * out_cols;
            for (j = 0; j < in_cols; j++)
                row[j] = xi[j] - m->mean[j];
            for (a = 0; a < out_cols; a++)
                oi[a] = dyn_ml_dot(row, m->comp + a * m->n_features,
                    m->n_features);
        }
    }
    dyn_matrix_free(&mx);
    free(row);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_matrix_to_js(ctx, out, rows, out_cols, flat);
    free(out);
    return result;
}

static JSValue dyn_pca_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FWD, 0);
}
static JSValue dyn_pca_transform_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FWD, 1);
}
static JSValue dyn_pca_fit_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FIT_FWD, 0);
}
static JSValue dyn_pca_inverse(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_INV, 0);
}

static JSValue dyn_pca_components(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->comp, m->n_components, m->n_features, 0);
}

static JSValue dyn_pca_mean_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->mean, m->n_features);
}

static JSValue dyn_pca_explained(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->var, m->n_components);
}

static JSValue dyn_pca_explained_ratio(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    double* tmp;
    size_t a;
    JSValue out;

    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    tmp = (double*)malloc(m->n_components * sizeof(double));
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
    JS_CFUNC_DEF("transformInto", 2, dyn_pca_transform_into),
    JS_CFUNC_DEF("fitTransform", 1, dyn_pca_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_pca_inverse),
    JS_CGETSET_DEF("components", dyn_pca_components, NULL),
    JS_CGETSET_DEF("mean", dyn_pca_mean_get, NULL),
    JS_CGETSET_DEF("explainedVariance", dyn_pca_explained, NULL),
    JS_CGETSET_DEF("explainedVarianceRatio", dyn_pca_explained_ratio, NULL),
};

typedef struct {
    int fitted;
    double var_smoothing;
    size_t n_features;
    size_t n_classes;
    double* classes;
    double* prior;
    double* mean;
    double* var;
    double* inv_var;
    double* logdet;
} dyn_nb_t;

static JSClassID dyn_nb_class_id;

static void dyn_nb_dispose(void* native)
{
    dyn_nb_t* m = (dyn_nb_t*)native;
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

static JSValue dyn_nb_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_nb_t* m;
    double vs = 1e-9;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &vs, argv[0]))
            return JS_EXCEPTION;
        if (!(vs >= 0.0))
            return JS_ThrowRangeError(ctx, "ml.GaussianNB: varSmoothing must not be negative");
    }
    m = (dyn_nb_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->var_smoothing = vs;
    return dyn_res_wrap(ctx, new_target, dyn_nb_class_id, m, dyn_nb_dispose);
}

static int dyn_nb_learn(JSContext* ctx, dyn_nb_t* m, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* w)
{
    double *classes = NULL, *prior = NULL, *mean = NULL, *var = NULL;
    double *inv_var = NULL, *logdet = NULL;
    double* count = NULL;
    size_t nc, c, i, j;
    double maxvar = 0.0, eps, wtot = 0.0;

    nc = dyn_ml_classes(ctx, y, rows, &classes);
    if (nc == 0)
        return -1;
    prior = (double*)calloc(nc, sizeof(double));
    mean = (double*)calloc(nc * cols, sizeof(double));
    var = (double*)calloc(nc * cols, sizeof(double));
    inv_var = (double*)calloc(nc * cols, sizeof(double));
    logdet = (double*)calloc(nc, sizeof(double));
    count = (double*)calloc(nc, sizeof(double));
    if (!prior || !mean || !var || !inv_var || !logdet || !count) {
        free(classes);
        free(prior);
        free(mean);
        free(var);
        free(inv_var);
        free(logdet);
        free(count);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
        double wi = w ? w[i] : 1.0;
        for (c = 0; c < nc; c++)
            if (classes[c] == y[i])
                break;
        if (c == nc) {
            free(classes);
            free(prior);
            free(mean);
            free(var);
            free(inv_var);
            free(logdet);
            free(count);
            JS_ThrowInternalError(ctx, "ml.fit: NaiveBayes: label outside class table");
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
        if (count[c] <= 0.0) {
            for (j = 0; j < cols; j++) {
                mean[c * cols + j] = 0.0;
                var[c * cols + j] = 0.0;
            }
            prior[c] = -DYN_INFINITY;
            continue;
        }
        for (j = 0; j < cols; j++) {
            double mu = mean[c * cols + j] / count[c];
            double v = var[c * cols + j] / count[c] - mu * mu;
            if (!isfinite(v)) {
                free(classes);
                free(prior);
                free(mean);
                free(var);
                free(inv_var);
                free(logdet);
                free(count);
                JS_ThrowRangeError(ctx,
                    "ml.fit: variance overflowed; scale X before GaussianNB");
                return -1;
            }
            if (v < 0.0)
                v = 0.0;
            mean[c * cols + j] = mu;
            var[c * cols + j] = v;
            if (v > maxvar)
                maxvar = v;
        }
        prior[c] = log(count[c] / wtot);
    }
    eps = m->var_smoothing * maxvar;
    if (!(eps > 0.0))
        eps = (m->var_smoothing > 0.0) ? m->var_smoothing : 0.0;
    for (c = 0; c < nc * cols; c++) {
        var[c] += eps;
        if (var[c] <= 0.0)
            var[c] = 1e-300;
    }
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
    free(m->classes);
    free(m->prior);
    free(m->mean);
    free(m->var);
    free(m->inv_var);
    free(m->logdet);
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

static JSValue dyn_nb_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_nb_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *w = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy(ctx, argv[0], argv[1], rows_arg, cols_arg,
            argc, argv, 2, &mx, &y, &w))
        return JS_EXCEPTION;
    m = (dyn_nb_t*)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    if (dyn_nb_learn(ctx, m, mx.data, y, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(y);
        free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    free(w);
    return JS_DupValue(ctx, this_val);
}

static double dyn_nb_score(const dyn_nb_t* m, const double* x, size_t c)
{
    const double* mu = m->mean + c * m->n_features;
    const double* iv = m->inv_var + c * m->n_features;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, d;
    size_t n = m->n_features, j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
        d = x[j + 1] - mu[j + 1];
        s1 += d * d * iv[j + 1];
        d = x[j + 2] - mu[j + 2];
        s2 += d * d * iv[j + 2];
        d = x[j + 3] - mu[j + 3];
        s3 += d * d * iv[j + 3];
    }
#endif
    for (; j < n; j++) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
    }
    return m->prior[c] - 0.5 * (m->logdet[c] + ((s0 + s1) + (s2 + s3)));
}

static JSValue dyn_nb_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int proba,
    int into)
{
    dyn_nb_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double *out = NULL, *sc = NULL;
    size_t rows, i, c, nc;
    int flat, as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_nb_t*)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    nc = m->n_classes;
    flat = !S && !mx.owned;
    if (into) {
        if (proba) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.predictInto: label prediction only; predictProba's rows x "
                "classes matrix keeps its Array form");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc((proba ? rows * nc : rows) * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    sc = (double*)malloc(nc * sizeof(double));
    if (!sc) {
        if (!into)
            free(out);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        free(sc);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
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
            double sum = 0.0;
            for (c = 0; c < nc; c++) {
                out[i * nc + c] = exp(sc[c] - best);
                sum += out[i * nc + c];
            }
            for (c = 0; c < nc; c++)
                out[i * nc + c] /= sum;
        }
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    free(sc);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = proba ? dyn_ml_matrix_to_js(ctx, out, rows, nc, flat)
                   : dyn_ml_predict_result(ctx, out, rows, as_f64);
    free(out);
    return result;
}

static JSValue dyn_nb_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 0, 0);
}
static JSValue dyn_nb_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 1, 0);
}
static JSValue dyn_nb_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 0, 1);
}

static JSValue dyn_nb_classes_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_nb_t* m = (dyn_nb_t*)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

static const JSCFunctionListEntry dyn_nb_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_nb_fit),
    JS_CFUNC_DEF("predict", 1, dyn_nb_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_nb_predict_into),
    JS_CFUNC_DEF("predictProba", 1, dyn_nb_predict_proba),
    JS_CGETSET_DEF("classes", dyn_nb_classes_get, NULL),
};

typedef struct {
    int fitted;
    int regressor;
    int weighted;
    size_t k;
    size_t rows, cols;
    double* X;
    double* y;
} dyn_knn_t;

static JSClassID dyn_knn_clf_class_id;
static JSClassID dyn_knn_reg_class_id;

static void dyn_knn_dispose(void* native)
{
    dyn_knn_t* m = (dyn_knn_t*)native;
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

static JSValue dyn_knn_new(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv,
    JSClassID class_id, int regressor)
{
    dyn_knn_t* m;
    int64_t k = 5;
    int weighted = 0;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k <= 0)
            return JS_ThrowRangeError(ctx, "dyna:ml k must be positive");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char* w = JS_ToCString(ctx, argv[1]);
        if (!w)
            return JS_EXCEPTION;
        if (!strcmp(w, "distance"))
            weighted = 1;
        else if (strcmp(w, "uniform")) {
            JS_FreeCString(ctx, w);
            return JS_ThrowTypeError(ctx,
                "dyna:ml weights must be \"uniform\" or \"distance\"");
        }
        JS_FreeCString(ctx, w);
    }
    m = (dyn_knn_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = (size_t)k;
    m->weighted = weighted;
    m->regressor = regressor;
    return dyn_res_wrap(ctx, new_target, class_id, m, dyn_knn_dispose);
}

static JSValue dyn_knn_clf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return dyn_knn_new(ctx, new_target, argc, argv, dyn_knn_clf_class_id, 0);
}

static JSValue dyn_knn_reg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return dyn_knn_new(ctx, new_target, argc, argv, dyn_knn_reg_class_id, 1);
}

static JSValue dyn_knn_fit_impl(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, JSClassID class_id)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "KNClassifier/KNRegressor"))
        return JS_EXCEPTION;
    dyn_knn_t* m;
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *Xcopy;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_no_w(ctx, argv[0], argv[1], rows_arg, cols_arg,
            &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_knn_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_EXCEPTION;
    }
    if (mx.rows < m->k) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_ThrowRangeError(ctx,
            "ml.fit: fit needs at least k rows (k = %u, rows = %u)",
            (unsigned)m->k, (unsigned)mx.rows);
    }
    Xcopy = (double*)malloc(mx.rows * mx.cols * sizeof(double));
    if (!Xcopy) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(Xcopy, mx.data, mx.rows * mx.cols * sizeof(double));
    free(m->X);
    free(m->y);
    m->X = Xcopy;
    m->y = y;
    m->rows = mx.rows;
    m->cols = mx.cols;
    m->fitted = 1;
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

static void dyn_knn_nearest(const dyn_knn_t* m, const double* q, double* nd,
    size_t* ni)
{
    size_t k = m->k, cols = m->cols, i, j, p;

    for (j = 0; j < k; j++) {
        nd[j] = HUGE_VAL;
        ni[j] = 0;
    }
    for (i = 0; i < m->rows; i++) {
        double d = dyn_ml_sqdist(q, m->X + i * cols, cols);
        if (d >= nd[k - 1])
            continue;
        for (p = k - 1; p > 0 && nd[p - 1] > d; p--) {
            nd[p] = nd[p - 1];
            ni[p] = ni[p - 1];
        }
        nd[p] = d;
        ni[p] = i;
    }
}

static JSValue dyn_knn_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int into)
{
    dyn_knn_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double *out = NULL, *nd = NULL;
    size_t* ni = NULL;
    size_t rows, i, a, b, k;
    int as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_knn_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->cols);
    }
    rows = S ? S->rows : mx.rows;
    k = m->k;
    if (rows > SIZE_MAX / sizeof(double) || k > SIZE_MAX / sizeof(double) || k > SIZE_MAX / sizeof(size_t)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: allocation size overflow");
    }
    if (into) {
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * sizeof(double));
    }
    nd = (double*)malloc(k * sizeof(double));
    ni = (size_t*)malloc(k * sizeof(size_t));
    if (!out || !nd || !ni) {
        if (!into)
            free(out);
        free(nd);
        free(ni);
        dyn_matrix_free(&mx);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        free(nd);
        free(ni);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* q = dyn_ml_row_at(&rs, i);
        dyn_knn_nearest(m, q, nd, ni);
        if (m->regressor) {
            double num = 0.0, den = 0.0;
            for (a = 0; a < k; a++) {
                double w = 1.0;
                if (m->weighted) {
                    double dist = sqrt(nd[a]);
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
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    free(nd);
    free(ni);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_predict_result(ctx, out, rows, as_f64);
    free(out);
    return result;
}

#define DYN_KNN_METHODS(prefix, class_id_var)                                      \
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,          \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_fit_impl(ctx, t, argc, argv, class_id_var);                 \
    }                                                                              \
    static JSValue prefix##_predict(JSContext* ctx, JSValueConst t, int argc,      \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_predict_impl(ctx, t, argc, argv, class_id_var, 0);          \
    }                                                                              \
    static JSValue prefix##_predict_into(JSContext* ctx, JSValueConst t, int argc, \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_predict_impl(ctx, t, argc, argv, class_id_var, 1);          \
    }

DYN_KNN_METHODS(dyn_knn_clf, dyn_knn_clf_class_id)
DYN_KNN_METHODS(dyn_knn_reg, dyn_knn_reg_class_id)

static const JSCFunctionListEntry dyn_knn_clf_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_clf_fit),
    JS_CFUNC_DEF("predict", 1, dyn_knn_clf_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_knn_clf_predict_into),
};

static const JSCFunctionListEntry dyn_knn_reg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_reg_fit),
    JS_CFUNC_DEF("predict", 1, dyn_knn_reg_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_knn_reg_predict_into),
};

typedef struct {
    double eps2;
    double eps;
    size_t min_pts;
    size_t rows;
    int* labels;
    int fitted;
    int n_clusters;
} dyn_dbscan_t;

_Static_assert(sizeof(dyn_dbscan_t) <= 48,
    "dyn_dbscan_t regained padding: reorder largest-first");

static JSClassID dyn_dbscan_class_id;

static void dyn_dbscan_dispose(void* native)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)native;
    if (m) {
        free(m->labels);
        free(m);
    }
}

static const JSClassDef dyn_dbscan_class = {
    "DBScan",
    .finalizer = dyn_res_finalizer,
};

static int dyn_dbscan_eps_ok(double eps)
{
    return isfinite(eps) && eps > 0.0 && isfinite(eps * eps);
}

static JSValue dyn_dbscan_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_dbscan_t* m;
    double eps = 0.5;
    int64_t min_pts = 5;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &eps, argv[0]))
            return JS_EXCEPTION;
        if (!dyn_dbscan_eps_ok(eps))
            return JS_ThrowRangeError(ctx,
                "ml.DBSCAN: eps must be positive, and finite with a square "
                "that does not overflow");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &min_pts, argv[1]))
            return JS_EXCEPTION;
        if (min_pts < 1)
            return JS_ThrowRangeError(ctx, "ml.DBSCAN: minPts must be at least 1");
    }
    m = (dyn_dbscan_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->eps = eps;
    m->eps2 = eps * eps;
    m->min_pts = (size_t)min_pts;
    return dyn_res_wrap(ctx, new_target, dyn_dbscan_class_id, m, dyn_dbscan_dispose);
}

#define DYN_DBSCAN_MAX_GRID_COLS 6
#define DYN_DBSCAN_MAX_GRID_CELLS 729
_Static_assert(3 * 3 * 3 * 3 * 3 * 3 == DYN_DBSCAN_MAX_GRID_CELLS,
    "DYN_DBSCAN_MAX_GRID_CELLS must be 3^DYN_DBSCAN_MAX_GRID_COLS");

typedef struct {
    size_t* head;
    size_t* next;
    uint32_t* mgen;
    size_t* mbkt;
    uint32_t gen;
    size_t slot_mask;
    size_t cap;
    size_t rows;
    size_t cols;
    double inv_eps;
} dyn_dbscan_grid_t;

static uint64_t dyn_dbscan_cell_hash(const int64_t* c, size_t cols)
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
    if (isnan(cd))
        cd = 0.0;
    else if (cd > 8.0e18)
        cd = 8.0e18;
    else if (cd < -8.0e18)
        cd = -8.0e18;
    return (int64_t)cd;
}

static int dyn_dbscan_grid_build(JSContext* ctx, dyn_dbscan_grid_t* g,
    const double* X, size_t rows, size_t cols,
    double eps)
{
    size_t cap = 16, i, d, cells = 1, slots = 16;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];

    while (cap < rows * 2 && cap <= SIZE_MAX / 2)
        cap *= 2;
    for (d = 0; d < cols; d++)
        cells *= 3;
    while (slots < cells * 2)
        slots *= 2;
    g->head = (size_t*)malloc(cap * sizeof(size_t));
    g->next = (size_t*)malloc(rows * sizeof(size_t));
    g->mgen = (uint32_t*)calloc(slots, sizeof(uint32_t));
    g->mbkt = (size_t*)malloc(slots * sizeof(size_t));
    if (!g->head || !g->next || !g->mgen || !g->mbkt) {
        free(g->head);
        free(g->next);
        free(g->mgen);
        free(g->mbkt);
        g->head = NULL;
        g->next = NULL;
        g->mgen = NULL;
        g->mbkt = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < cap; i++)
        g->head[i] = SIZE_MAX;
    g->cap = cap;
    g->rows = rows;
    g->gen = 1;
    g->slot_mask = slots - 1;
    g->cols = cols;
    g->inv_eps = 1.0 / eps;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
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

static size_t dyn_dbscan_grid_region(dyn_dbscan_grid_t* g,
    const double* X, size_t p, double eps2,
    size_t* out)
{
    const double* xp = X + p * g->cols;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];
    int64_t off[DYN_DBSCAN_MAX_GRID_COLS] = { 0 };
    size_t n = 0, d, k, cap = g->rows;
    uint32_t gen;

    if (++g->gen == 0) {
        memset(g->mgen, 0, (g->slot_mask + 1) * sizeof(uint32_t));
        g->gen = 1;
    }
    gen = g->gen;
    for (d = 0; d < g->cols; d++)
        c[d] = dyn_dbscan_coord(xp[d], g->inv_eps);
    for (;;) {
        int64_t cc[DYN_DBSCAN_MAX_GRID_COLS];
        for (d = 0; d < g->cols; d++)
            cc[d] = c[d] + off[d] - 1;
        {
            size_t b = (size_t)dyn_dbscan_cell_hash(cc, g->cols) & (g->cap - 1);
            size_t hd = g->head[b];
            if (hd != SIZE_MAX) {
                int claimed = 0;
                for (k = hd; k != SIZE_MAX; k = g->next[k]) {
                    if (dyn_ml_sqdist(xp, X + k * g->cols, g->cols) > eps2)
                        continue;
                    if (!claimed) {
                        size_t s = b & g->slot_mask;
                        while (g->mgen[s] == gen && g->mbkt[s] != b)
                            s = (s + 1) & g->slot_mask;
                        if (g->mgen[s] == gen)
                            break;
                        g->mgen[s] = gen;
                        g->mbkt[s] = b;
                        claimed = 1;
                    }
                    if (n >= cap)
                        break;
                    out[n++] = k;
                }
            }
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

static size_t dyn_dbscan_region(const double* X, size_t rows, size_t cols,
    size_t p, double eps2, size_t* out)
{
    const double* xp = X + p * cols;
    size_t n = 0, i;
    for (i = 0; i < rows; i++)
        if (dyn_ml_sqdist(xp, X + i * cols, cols) <= eps2)
            out[n++] = i;
    return n;
}

static int dyn_dbscan_run(JSContext* ctx, dyn_dbscan_t* m, const double* X,
    size_t rows, size_t cols, int* labels)
{
    size_t *neigh = NULL, *queue = NULL, *seed = NULL;
    unsigned char* queued = NULL;
    dyn_dbscan_grid_t grid;
    size_t i, qn, qi, nn, j, sn;
    int cluster = 0, have_grid;

    neigh = (size_t*)malloc(rows * sizeof(size_t));
    seed = (size_t*)malloc(rows * sizeof(size_t));
    queue = (size_t*)malloc(rows * sizeof(size_t));
    queued = (unsigned char*)calloc(rows, 1);
    if (!neigh || !seed || !queue || !queued) {
        free(neigh);
        free(seed);
        free(queue);
        free(queued);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    have_grid = (cols <= DYN_DBSCAN_MAX_GRID_COLS && rows >= (size_t)1 << (cols * 2));
    memset(&grid, 0, sizeof(grid));
    if (have_grid && dyn_dbscan_grid_build(ctx, &grid, X, rows, cols, m->eps)) {
        free(neigh);
        free(seed);
        free(queue);
        free(queued);
        return -1;
    }
    for (i = 0; i < rows; i++)
        labels[i] = -1;

    for (i = 0; i < rows; i++) {
        if (labels[i] != -1 || queued[i])
            continue;
        nn = have_grid
            ? dyn_dbscan_grid_region(&grid, X, i, m->eps2, neigh)
            : dyn_dbscan_region(X, rows, cols, i, m->eps2, neigh);
        if (nn < m->min_pts)
            continue;
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
            if (labels[q] == -1)
                labels[q] = cluster;
            sn = have_grid
                ? dyn_dbscan_grid_region(&grid, X, q, m->eps2, seed)
                : dyn_dbscan_region(X, rows, cols, q, m->eps2, seed);
            if (sn < m->min_pts)
                continue;
            for (j = 0; j < sn; j++) {
                size_t s = seed[j];
                if (queued[s] || labels[s] != -1)
                    continue;
                queued[s] = 1;
                queue[qn++] = s;
            }
        }
        cluster++;
    }
    free(grid.head);
    free(grid.next);
    free(grid.mgen);
    free(grid.mbkt);
    free(neigh);
    free(seed);
    free(queue);
    free(queued);
    return cluster;
}

static JSValue dyn_dbscan_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "DBScan"))
        return JS_EXCEPTION;
    dyn_dbscan_t* m;
    dyn_matrix_t mx = { 0 };
    int* labels;
    int nc;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    labels = (int*)malloc(mx.rows * sizeof(int));
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

static JSValue dyn_dbscan_labels(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_ints_to_js(ctx, m->labels, m->rows);
}

static JSValue dyn_dbscan_nclusters(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, m->fitted ? m->n_clusters : 0);
}

static JSValue dyn_dbscan_eps(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
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

typedef struct {
    int fitted;
    int minmax;
    size_t n_features;
    double* centre;
    double* scale;
    double* spread;
} dyn_scaler_t;

static JSClassID dyn_stdscaler_class_id;
static JSClassID dyn_minmax_class_id;

static void dyn_scaler_dispose(void* native)
{
    dyn_scaler_t* s = (dyn_scaler_t*)native;
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

static JSValue dyn_scaler_new(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, int minmax)
{
    dyn_scaler_t* s = (dyn_scaler_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->minmax = minmax;
    return dyn_res_wrap(ctx, new_target, class_id, s, dyn_scaler_dispose);
}

static JSValue dyn_stdscaler_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_scaler_new(ctx, new_target, dyn_stdscaler_class_id, 0);
}

static JSValue dyn_minmax_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_scaler_new(ctx, new_target, dyn_minmax_class_id, 1);
}

static int dyn_scaler_learn(JSContext* ctx, dyn_scaler_t* s, const double* X,
    size_t rows, size_t cols, const double* w)
{
    double *centre, *scale, *spread, *sumsq = NULL;
    size_t i, j;

    centre = (double*)calloc(cols, sizeof(double));
    scale = (double*)malloc(cols * sizeof(double));
    spread = (double*)malloc(cols * sizeof(double));
    if (!s->minmax)
        sumsq = (double*)calloc(cols, sizeof(double));
    if (!centre || !scale || !spread || (!s->minmax && !sumsq)) {
        free(centre);
        free(scale);
        free(spread);
        free(sumsq);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (s->minmax) {
        for (j = 0; j < cols; j++) {
            centre[j] = X[j];
            spread[j] = X[j];
        }
        for (i = 1; i < rows; i++) {
            const double* xi = X + i * cols;
            for (j = 0; j < cols; j++) {
                if (xi[j] < centre[j])
                    centre[j] = xi[j];
                if (xi[j] > spread[j])
                    spread[j] = xi[j];
            }
        }
        for (j = 0; j < cols; j++) {
            double range = spread[j] - centre[j];
            spread[j] = range;
            scale[j] = (range > 0.0) ? 1.0 / range : 1.0;
        }
    } else {
        double wtot = (double)rows;
        size_t i2;
        if (w) {
            wtot = 0.0;
            for (i = 0; i < rows; i++) {
                const double* xi = X + i * cols;
                double wi = w[i];
                wtot += wi;
                for (j = 0; j < cols; j++)
                    centre[j] += wi * xi[j];
            }
        } else {
            for (i = 0; i < rows; i++) {
                const double* xi = X + i * cols;
                for (j = 0; j < cols; j++)
                    centre[j] += xi[j];
            }
        }
        for (j = 0; j < cols; j++)
            centre[j] /= wtot;
        if (w) {
            for (i2 = 0; i2 < rows; i2++) {
                const double* xi = X + i2 * cols;
                double wi = w[i2];
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - centre[j];
                    sumsq[j] += wi * d * d;
                }
            }
        } else {
            for (i2 = 0; i2 < rows; i2++) {
                const double* xi = X + i2 * cols;
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - centre[j];
                    sumsq[j] += d * d;
                }
            }
        }
        for (j = 0; j < cols; j++) {
            double var = sumsq[j] / wtot;
            double sd;
            if (!isfinite(var)) {
                free(centre);
                free(scale);
                free(spread);
                free(sumsq);
                JS_ThrowRangeError(ctx,
                    "ml.fit: variance overflowed; scale X before StandardScaler");
                return -1;
            }
            if (var < 0.0)
                var = 0.0;
            sd = sqrt(var);
            spread[j] = (sd > 0.0) ? sd : 1.0;
            scale[j] = (sd > 0.0) ? 1.0 / sd : 1.0;
        }
        free(sumsq);
    }
    free(s->centre);
    free(s->scale);
    free(s->spread);
    s->centre = centre;
    s->scale = scale;
    s->spread = spread;
    s->n_features = cols;
    s->fitted = 1;
    return 0;
}

static JSValue dyn_scaler_fit_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id)
{
    dyn_scaler_t* s;
    dyn_matrix_t mx = { 0 };
    double* w = NULL;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    int minmax;

    if (dyn_ml_ingest_X_w(ctx, argv[0], rows_arg, cols_arg, argc, argv, 1,
            &mx, &w))
        return JS_EXCEPTION;
    s = (dyn_scaler_t*)dyn_res_native(ctx, this_val, class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    if (!s) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    minmax = s->minmax;
    if (minmax && w) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_ThrowTypeError(ctx,
            "ml.fit: MinMaxScaler has no weighted fit: min and max are order "
            "statistics and no positive weight changes them. Drop the rows you "
            "meant to exclude, or use StandardScaler, which weights its mean "
            "and variance.");
    }
    if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(w);
    return JS_DupValue(ctx, this_val);
}

enum { DYN_SCALE_FWD,
    DYN_SCALE_INV,
    DYN_SCALE_FIT_FWD };

static JSValue dyn_scaler_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int mode, int into)
{
    dyn_scaler_t* s;
    dyn_matrix_t mx = { 0 };
    double* out = NULL;
    size_t i, j, rows, cols;
    int flat;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    s = (dyn_scaler_t*)dyn_res_native(ctx, this_val, class_id);
    if (!s) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (mode == DYN_SCALE_FIT_FWD) {
        if (dyn_ml_check_finite(ctx, &mx, NULL)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
        if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, NULL)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else if (!s->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.transform: transform before fit");
    } else if (mx.cols != s->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.transform: X has %u features, scaler expects %u",
            (unsigned)mx.cols, (unsigned)s->n_features);
    }
    rows = mx.rows;
    cols = mx.cols;
    flat = !mx.owned;
    if (into) {
        if (mode != DYN_SCALE_FWD) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.transformInto: forward transform only; inverseTransform "
                "keeps its shape-in shape-out result");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows * cols, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc((rows * cols != 0 ? rows * cols : 1) * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    for (i = 0; i < rows; i++) {
        const double* xi = mx.data + i * cols;
        double* oi = out + i * cols;
        if (mode == DYN_SCALE_INV)
            for (j = 0; j < cols; j++)
                oi[j] = xi[j] * s->spread[j] + s->centre[j];
        else
            for (j = 0; j < cols; j++)
                oi[j] = (xi[j] - s->centre[j]) * s->scale[j];
    }
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = dyn_ml_matrix_to_js(ctx, out, rows, cols, flat);
    free(out);
    return result;
}

#define DYN_SCALER_METHODS(prefix, class_id_var)                                         \
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,                \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_fit_impl(ctx, t, argc, argv, class_id_var);                    \
    }                                                                                    \
    static JSValue prefix##_transform(JSContext* ctx, JSValueConst t, int argc,          \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FWD, 0);     \
    }                                                                                    \
    static JSValue prefix##_into(JSContext* ctx, JSValueConst t, int argc,               \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FWD, 1);     \
    }                                                                                    \
    static JSValue prefix##_inverse(JSContext* ctx, JSValueConst t, int argc,            \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_INV, 0);     \
    }                                                                                    \
    static JSValue prefix##_fit_transform(JSContext* ctx, JSValueConst t,                \
        int argc, JSValueConst* argv)                                                    \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FIT_FWD, 0); \
    }

DYN_SCALER_METHODS(dyn_stdscaler, dyn_stdscaler_class_id)
DYN_SCALER_METHODS(dyn_minmax, dyn_minmax_class_id)

static JSValue dyn_scaler_stat(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id, int which)
{
    dyn_scaler_t* s = (dyn_scaler_t*)dyn_res_native(ctx, this_val, class_id);
    const double* v;
    if (!s)
        return JS_EXCEPTION;
    if (!s->fitted)
        return JS_NewArray(ctx);
    v = which == 0 ? s->centre : s->spread;
    return dyn_ml_doubles_to_js(ctx, v, s->n_features);
}

static JSValue dyn_stdscaler_mean(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 0);
}
static JSValue dyn_stdscaler_std(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 1);
}
static JSValue dyn_minmax_min(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_minmax_class_id, 0);
}

static JSValue dyn_minmax_max(JSContext* ctx, JSValueConst this_val)
{
    dyn_scaler_t* s = (dyn_scaler_t*)dyn_res_native(ctx, this_val, dyn_minmax_class_id);
    double* tmp;
    size_t j;
    JSValue out;

    if (!s)
        return JS_EXCEPTION;
    if (!s->fitted)
        return JS_NewArray(ctx);
    tmp = (double*)malloc(s->n_features * sizeof(double));
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
    JS_CFUNC_DEF("transformInto", 2, dyn_stdscaler_into),
    JS_CFUNC_DEF("fitTransform", 1, dyn_stdscaler_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_stdscaler_inverse),
    JS_CGETSET_DEF("mean", dyn_stdscaler_mean, NULL),
    JS_CGETSET_DEF("std", dyn_stdscaler_std, NULL),
};

static const JSCFunctionListEntry dyn_minmax_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_minmax_fit),
    JS_CFUNC_DEF("transform", 1, dyn_minmax_transform),
    JS_CFUNC_DEF("transformInto", 2, dyn_minmax_into),
    JS_CFUNC_DEF("fitTransform", 1, dyn_minmax_fit_transform),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_minmax_inverse),
    JS_CGETSET_DEF("dataMin", dyn_minmax_min, NULL),
    JS_CGETSET_DEF("dataMax", dyn_minmax_max, NULL),
};

typedef enum { DYN_SVM_LINEAR,
    DYN_SVM_RBF,
    DYN_SVM_POLY } dyn_svm_kernel_t;

typedef struct {
    double* alpha;
    double* sv;
    double b;
    size_t n_sv;
} dyn_svm_bin_t;

typedef struct {
    int fitted;
    dyn_svm_kernel_t kernel;
    double gamma;
    double coef0;
    int degree;
    double C;
    double tol;
    size_t max_iter;
    size_t cols;
    size_t n_classes;
    double* classes;
    dyn_svm_bin_t* bin;
    size_t n_bin;
} dyn_svm_t;

static JSClassID dyn_svm_class_id;

static void dyn_svm_dispose(void* native)
{
    dyn_svm_t* m = (dyn_svm_t*)native;
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

static JSValue dyn_svm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_svm_t* m;
    dyn_svm_kernel_t kern = DYN_SVM_RBF;
    double C = 1.0, gamma = 0.0, coef0 = 0.0, tol = 1e-3;
    size_t max_iter = 1000, degree_holder = 3;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst o = argv[0];
        if (dyn_opts_strict(ctx, o, ml_svm_keys, 7))
            return JS_EXCEPTION;
        JSValue kv = JS_GetPropertyStr(ctx, o, "kernel");
        if (JS_IsException(kv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(kv)) {
            const char* k = JS_ToCString(ctx, kv);
            JS_FreeValue(ctx, kv);
            if (!k)
                return JS_EXCEPTION;
            if (!strcmp(k, "linear"))
                kern = DYN_SVM_LINEAR;
            else if (!strcmp(k, "rbf"))
                kern = DYN_SVM_RBF;
            else if (!strcmp(k, "poly"))
                kern = DYN_SVM_POLY;
            else {
                JS_FreeCString(ctx, k);
                return JS_ThrowTypeError(ctx,
                    "dyna:ml kernel must be \"linear\", \"rbf\" or \"poly\"");
            }
            JS_FreeCString(ctx, k);
        } else {
            JS_FreeValue(ctx, kv);
        }
        if (dyn_opt_double(ctx, o, "C", &C, 1e-12, 1e12) || dyn_opt_double(ctx, o, "gamma", &gamma, 0.0, 1e12) || dyn_opt_double(ctx, o, "coef0", &coef0, -1e12, 1e12) || dyn_opt_double(ctx, o, "tol", &tol, 1e-15, 1.0) || dyn_opt_size(ctx, o, "degree", &degree_holder, 1) || dyn_opt_size(ctx, o, "maxIter", &max_iter, 1))
            return JS_EXCEPTION;
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "dyna:ml expected an options object");
    }
    if (degree_holder > 1000)
        return JS_ThrowRangeError(ctx, "dyna:ml degree must be at most 1000");
    if (max_iter > DYN_ML_MAX_ITERS)
        return JS_ThrowRangeError(ctx, "dyna:ml maxIter must be at most %u",
            (unsigned)DYN_ML_MAX_ITERS);
    m = (dyn_svm_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->kernel = kern;
    m->C = C;
    m->gamma = gamma;
    m->coef0 = coef0;
    m->degree = (int)degree_holder;
    m->tol = tol;
    m->max_iter = max_iter;
    return dyn_res_wrap(ctx, new_target, dyn_svm_class_id, m, dyn_svm_dispose);
}

static double dyn_svm_k(const dyn_svm_t* m, const double* a, const double* b,
    size_t d, double gamma)
{
    switch ((int)(m->kernel)) {
    case DYN_SVM_LINEAR:
        return dyn_ml_dot(a, b, d);
    case DYN_SVM_POLY: {
        double base = gamma * dyn_ml_dot(a, b, d) + m->coef0;
        double r = 1.0;
        int e;
        for (e = 0; e < m->degree; e++)
            r *= base;
        return r;
    }
    default:
        return exp(-gamma * dyn_ml_sqdist(a, b, d));
    }
}

static int dyn_svm_train_bin(JSContext* ctx, dyn_svm_t* m, const double* X,
    const double* yb, size_t rows, size_t cols,
    double gamma, dyn_svm_bin_t* out, uint64_t* rng)
{
    double *alpha = NULL, *f = NULL, *krow_i = NULL, *krow_j = NULL;
    double b = 0.0;
    size_t iter, i, j, nsv, s;
    int rc = -1;

    alpha = (double*)calloc(rows, sizeof(double));
    f = (double*)calloc(rows, sizeof(double));
    krow_i = (double*)malloc(rows * sizeof(double));
    krow_j = (double*)malloc(rows * sizeof(double));
    if (!alpha || !f || !krow_i || !krow_j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (iter = 0; iter < m->max_iter; iter++) {
        size_t changed = 0;
        for (i = 0; i < rows; i++) {
            double Ei = f[i] - yb[i];
            if (JS_CheckInterrupt(ctx))
                goto done;
            double ri = yb[i] * Ei;
            if (!((ri < -m->tol && alpha[i] < m->C) || (ri > m->tol && alpha[i] > 0.0)))
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
                        "ml.fit: polynomial kernel overflowed; scale X or lower degree");
                    goto done;
                }
                eta = kii + kjj - 2.0 * kij;
                if (!isfinite(eta) || eta <= 1e-15)
                    continue;
                aj_new = aj_old + yb[j] * (Ei - Ej) / eta;
                if (aj_new > H)
                    aj_new = H;
                if (aj_new < L)
                    aj_new = L;
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
                for (s = 0; s < rows; s++) {
                    krow_i[s] = dyn_svm_k(m, X + i * cols, X + s * cols, cols, gamma);
                    krow_j[s] = dyn_svm_k(m, X + j * cols, X + s * cols, cols, gamma);
                    if (!isfinite(krow_i[s]) || !isfinite(krow_j[s])) {
                        JS_ThrowRangeError(ctx,
                            "ml.fit: polynomial kernel overflowed; scale X or lower degree");
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
            break;
    }
    nsv = 0;
    for (i = 0; i < rows; i++)
        if (alpha[i] > 1e-12)
            nsv++;
    out->alpha = (double*)malloc((nsv ? nsv : 1) * sizeof(double));
    out->sv = (double*)malloc((nsv ? nsv * cols : 1) * sizeof(double));
    if (!out->alpha || !out->sv) {
        free(out->alpha);
        free(out->sv);
        out->alpha = NULL;
        out->sv = NULL;
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    nsv = 0;
    for (i = 0; i < rows; i++) {
        if (!(alpha[i] > 1e-12))
            continue;
        out->alpha[nsv] = alpha[i] * yb[i];
        memcpy(out->sv + nsv * cols, X + i * cols, cols * sizeof(double));
        nsv++;
    }
    out->n_sv = nsv;
    out->b = b;
    rc = 0;
done:
    free(alpha);
    free(f);
    free(krow_i);
    free(krow_j);
    return rc;
}

static double dyn_svm_decide(const dyn_svm_t* m, const dyn_svm_bin_t* bin,
    const double* x, double gamma)
{
    double s = bin->b;
    size_t i;
    for (i = 0; i < bin->n_sv; i++)
        s += bin->alpha[i] * dyn_svm_k(m, bin->sv + i * m->cols, x, m->cols, gamma);
    return s;
}

static int dyn_svm_learn(JSContext* ctx, dyn_svm_t* m, const double* X,
    const double* y, size_t rows, size_t cols)
{
    double *classes = NULL, *yb = NULL;
    dyn_svm_bin_t* bin = NULL;
    size_t nc, nb, k, i;
    double gamma;
    uint64_t rng = 987654321ULL;
    int rc = -1;

    nc = dyn_ml_classes(ctx, y, rows, &classes);
    if (nc == 0)
        return -1;
    if (nc < 2) {
        free(classes);
        JS_ThrowRangeError(ctx, "ml.fit: SVC needs at least two classes");
        return -1;
    }
    if (nc > 256) {
        free(classes);
        JS_ThrowRangeError(ctx, "ml.fit: SVC supports at most 256 classes, found %u",
            (unsigned)nc);
        return -1;
    }
    gamma = (m->gamma > 0.0) ? m->gamma : 1.0 / (double)cols;
    nb = (nc == 2) ? 1 : nc;
    bin = (dyn_svm_bin_t*)calloc(nb, sizeof(*bin));
    yb = (double*)malloc(rows * sizeof(double));
    if (!bin || !yb) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    m->cols = cols;
    for (k = 0; k < nb; k++) {
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

static JSValue dyn_svm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "SVC"))
        return JS_EXCEPTION;
    dyn_svm_t* m;
    dyn_matrix_t mx = { 0 };
    double* y = NULL;
    JSValueConst rows_arg, cols_arg;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y) requires two arguments");
    rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    if (dyn_ml_ingest_Xy_no_w(ctx, argv[0], argv[1], rows_arg, cols_arg,
            &mx, &y))
        return JS_EXCEPTION;
    m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_EXCEPTION;
    }
    if (dyn_svm_learn(ctx, m, mx.data, y, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx);
        free(y);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(y);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_svm_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int decision,
    int into)
{
    dyn_svm_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, k, ocols;
    int flat, as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->cols);
    }
    rows = S ? S->rows : mx.rows;
    flat = !S && !mx.owned;
    ocols = decision ? m->n_bin : 1;
    if (rows > (SIZE_MAX / sizeof(double)) / ocols) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: data too large for SVC predict");
    }
    if (into) {
        if (decision) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.decisionInto: not provided; read decisionFunction's output "
                "columns from predict/predictInto instead");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * ocols * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < rows; i++) {
        const double* xi = dyn_ml_row_at(&rs, i);
#define DYN_SVM_REFUSE_NONFINITE(dv)                                                  \
    do {                                                                              \
        if (!isfinite(dv)) {                                                          \
            if (!into)                                                                \
                free(out);                                                            \
            dyn_ml_row_src_free(&rs);                                                 \
            dyn_matrix_free(&mx);                                                     \
            return JS_ThrowRangeError(ctx,                                            \
                "ml.predict: polynomial kernel overflowed; scale X or lower degree"); \
        }                                                                             \
    } while (0)
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
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = (decision && ocols > 1)
        ? dyn_ml_matrix_to_js(ctx, out, rows, ocols, flat)
        : dyn_ml_predict_result(ctx, out, rows, as_f64 || (decision && flat));
    free(out);
    return result;
}

static JSValue dyn_svm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 0, 0);
}
static JSValue dyn_svm_decision(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 1, 0);
}
static JSValue dyn_svm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_svm_predict_impl(ctx, t, argc, argv, 0, 1);
}

static JSValue dyn_svm_nsv(JSContext* ctx, JSValueConst this_val)
{
    dyn_svm_t* m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    size_t k, tot = 0;
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewInt32(ctx, 0);
    for (k = 0; k < m->n_bin; k++)
        tot += m->bin[k].n_sv;
    return JS_NewInt64(ctx, (int64_t)tot);
}

static JSValue dyn_svm_classes_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_svm_t* m = (dyn_svm_t*)dyn_res_native(ctx, this_val, dyn_svm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

static const JSCFunctionListEntry dyn_svm_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_svm_fit),
    JS_CFUNC_DEF("predict", 1, dyn_svm_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_svm_predict_into),
    JS_CFUNC_DEF("decisionFunction", 1, dyn_svm_decision),
    JS_CGETSET_DEF("nSupportVectors", dyn_svm_nsv, NULL),
    JS_CGETSET_DEF("classes", dyn_svm_classes_get, NULL),
};

typedef struct {
    int fitted;
    size_t k;
    size_t cols;
    size_t max_iter;
    double tol;
    double reg;
    uint64_t seed;
    double* weight;
    double* mean;
    double* var;
    double* inv_var;
    double* lognorm;
    double loglik;
    size_t n_iter;
} dyn_gmm_t;

static JSClassID dyn_gmm_class_id;

static void dyn_gmm_dispose(void* native)
{
    dyn_gmm_t* m = (dyn_gmm_t*)native;
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

static JSValue dyn_gmm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_gmm_t* m;
    int64_t k = 3;
    size_t max_iter = 200, seed_holder = 12345;
    double tol = 1e-3, reg = 1e-6;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k < 1)
            return JS_ThrowRangeError(ctx, "ml.GaussianMixture: nComponents must be at least 1");
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], ml_gmm_keys, 4))
            return JS_EXCEPTION;
        if (dyn_opt_size(ctx, argv[1], "maxIter", &max_iter, 1) || dyn_opt_size(ctx, argv[1], "seed", &seed_holder, 0) || dyn_opt_double(ctx, argv[1], "tol", &tol, 0.0, 1e12) || dyn_opt_double(ctx, argv[1], "regCovar", &reg, 0.0, 1e12))
            return JS_EXCEPTION;
    } else if (argc > 1 && !JS_IsUndefined(argv[1])) {
        return JS_ThrowTypeError(ctx, "ml.GaussianMixture: expected an options object");
    }
    if (max_iter > DYN_ML_MAX_EM_ITERS)
        return JS_ThrowRangeError(ctx, "ml.GaussianMixture: maxIter must be at most %u",
            (unsigned)DYN_ML_MAX_EM_ITERS);
    m = (dyn_gmm_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = (size_t)k;
    m->max_iter = max_iter;
    m->tol = tol;
    m->reg = reg;
    m->seed = (uint64_t)seed_holder;
    return dyn_res_wrap(ctx, new_target, dyn_gmm_class_id, m, dyn_gmm_dispose);
}

static double dyn_gmm_logpdf(const dyn_gmm_t* m, const double* x, size_t c)
{
    const double* mu = m->mean + c * m->cols;
    const double* iv = m->inv_var + c * m->cols;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, d;
    size_t n = m->cols, j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
        d = x[j + 1] - mu[j + 1];
        s1 += d * d * iv[j + 1];
        d = x[j + 2] - mu[j + 2];
        s2 += d * d * iv[j + 2];
        d = x[j + 3] - mu[j + 3];
        s3 += d * d * iv[j + 3];
    }
#endif
    for (; j < n; j++) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
    }
    return m->lognorm[c] - 0.5 * ((s0 + s1) + (s2 + s3));
}

static void dyn_gmm_refresh(dyn_gmm_t* m)
{
    size_t c, j;
    for (c = 0; c < m->k; c++) {
        double ld = 0.0;
        for (j = 0; j < m->cols; j++) {
            double v = m->var[c * m->cols + j];
            if (!(v > 0.0))
                v = 1e-12;
            m->inv_var[c * m->cols + j] = 1.0 / v;
            ld += log(6.283185307179586476925286766559 * v);
        }
        m->lognorm[c] = log(m->weight[c]) - 0.5 * ld;
    }
}

static double dyn_gmm_estep(const dyn_gmm_t* m, const double* X, size_t rows,
    double* resp)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * m->cols;
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
        total += best + log(sum);
    }
    return total;
}

static int dyn_gmm_learn(JSContext* ctx, dyn_gmm_t* m, const double* X,
    size_t rows, size_t cols)
{
    double *weight = NULL, *mean = NULL, *var = NULL, *resp = NULL;
    double* nk_all = NULL;
    double *inv_var = NULL, *lognorm = NULL, *nearest = NULL;
    int* labels = NULL;
    size_t k = m->k, i, c, j, it;
    uint64_t rng = m->seed;
    double prev = -HUGE_VAL, ll = -HUGE_VAL;
    int rc = -1;

    if (rows < k) {
        JS_ThrowRangeError(ctx,
            "ml.fit: GaussianMixture needs at least nComponents rows");
        return -1;
    }
    if (rows > (SIZE_MAX / sizeof(double)) / k) {
        JS_ThrowRangeError(ctx, "ml.fit: data too large for GaussianMixture");
        return -1;
    }
    weight = (double*)malloc(k * sizeof(double));
    mean = (double*)malloc(k * cols * sizeof(double));
    var = (double*)malloc(k * cols * sizeof(double));
    inv_var = (double*)malloc(k * cols * sizeof(double));
    lognorm = (double*)malloc(k * sizeof(double));
    resp = (double*)malloc(rows * k * sizeof(double));
    nk_all = (double*)malloc((k ? k : 1) * sizeof(double));
    nearest = (double*)malloc(rows * sizeof(double));
    labels = (int*)malloc(rows * sizeof(int));
    if (!weight || !mean || !var || !inv_var || !lognorm || !resp || !nearest || !labels || !nk_all) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
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
        weight[c] = (cnt > 0.0 ? cnt : 1.0) / (double)rows;
    }
    dyn_gmm_refresh(m);
    for (it = 0; it < m->max_iter; it++) {
        if (JS_CheckInterrupt(ctx))
            goto done;
        ll = dyn_gmm_estep(m, X, rows, resp);
        for (c = 0; c < k; c++)
            nk_all[c] = 0.0;
        for (i = 0; i < rows; i++)
            for (c = 0; c < k; c++)
                nk_all[c] += resp[i * k + c];

        for (c = 0; c < k; c++)
            for (j = 0; j < cols; j++)
                mean[c * cols + j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double* xi = X + i * cols;
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
            const double* xi = X + i * cols;
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
                weight[c] = 1e-300;
                continue;
            }
            for (j = 0; j < cols; j++)
                var[c * cols + j] = var[c * cols + j] / nk_all[c] + m->reg;
            weight[c] = nk_all[c] / (double)rows;
        }
        dyn_gmm_refresh(m);
        m->n_iter = it + 1;
        if (it > 0 && fabs(ll - prev) < m->tol * fabs(ll ? ll : 1.0))
            break;
        prev = ll;
    }
    m->loglik = dyn_gmm_estep(m, X, rows, resp);
    m->fitted = 1;
    weight = NULL;
    mean = NULL;
    var = NULL;
    inv_var = NULL;
    lognorm = NULL;
    rc = 0;
done:
    if (rc != 0) {
        free(weight);
        free(mean);
        free(var);
        free(inv_var);
        free(lognorm);
        m->weight = NULL;
        m->mean = NULL;
        m->var = NULL;
        m->inv_var = NULL;
        m->lognorm = NULL;
    }
    free(resp);
    free(nearest);
    free(labels);
    free(nk_all);
    return rc;
}

static JSValue dyn_gmm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "GaussianMixture"))
        return JS_EXCEPTION;
    dyn_gmm_t* m;
    dyn_matrix_t mx = { 0 };
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    {
        double *ow = m->weight, *om = m->mean, *ov = m->var;
        double *oi = m->inv_var, *ol = m->lognorm;
        size_t ocols = m->cols, oiter = m->n_iter;
        double oll = m->loglik;
        if (dyn_gmm_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
            m->weight = ow;
            m->mean = om;
            m->var = ov;
            m->inv_var = oi;
            m->lognorm = ol;
            m->cols = ocols;
            m->n_iter = oiter;
            m->loglik = oll;
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
        free(ow);
        free(om);
        free(ov);
        free(oi);
        free(ol);
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_gmm_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int proba,
    int into)
{
    dyn_gmm_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, c;
    int flat, as_f64 = 0;
    JSValue result;
    JSValueConst xa, rows_arg, cols_arg;

    dyn_ml_into_args(into, argc, argv, &xa, &rows_arg, &cols_arg);
    if (!into && dyn_ml_parse_as_opt(ctx, argc, argv, 1, &as_f64))
        return JS_EXCEPTION;
    if (JS_GetClassID(xa) == dyn_csr_class_id) {
        S = dyn_ml_as_csr(ctx, xa);
        if (!S)
            return JS_EXCEPTION;
    } else if (dyn_ml_ingest_X(ctx, xa, rows_arg, cols_arg, &mx)) {
        return JS_EXCEPTION;
    }
    if (S ? dyn_ml_check_csr_values_finite(ctx, S)
          : dyn_ml_check_finite_fast(ctx, &mx)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return JS_ThrowInternalError(ctx, "ml.predict: predict before fit");
    }
    if ((S ? S->cols : mx.cols) != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->cols);
    }
    rows = S ? S->rows : mx.rows;
    flat = !S && !mx.owned;
    if (proba && (m->k == 0 || rows > (SIZE_MAX / sizeof(double)) / m->k)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: data too large for predictProba");
    }
    if (into) {
        if (proba) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.predictInto: label prediction only; predictProba's rows x "
                "components matrix keeps its Array form");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * (proba ? m->k : 1) * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (proba) {
        for (i = 0; i < rows; i++)
            dyn_gmm_estep(m, dyn_ml_row_at(&rs, i), 1, out + i * m->k);
    } else {
        double* sc = (double*)malloc(m->k * sizeof(double));
        if (!sc) {
            if (!into)
                free(out);
            dyn_ml_row_src_free(&rs);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < rows; i++) {
            const double* xi = dyn_ml_row_at(&rs, i);
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
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = proba ? dyn_ml_matrix_to_js(ctx, out, rows, m->k, flat)
                   : dyn_ml_predict_result(ctx, out, rows, as_f64);
    free(out);
    return result;
}

static JSValue dyn_gmm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 0, 0);
}
static JSValue dyn_gmm_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 1, 0);
}
static JSValue dyn_gmm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 0, 1);
}

static JSValue dyn_gmm_weights(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->weight, m->k);
}

static JSValue dyn_gmm_means(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->mean, m->k, m->cols, 0);
}

static JSValue dyn_gmm_variances(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->var, m->k, m->cols, 0);
}

static JSValue dyn_gmm_loglik(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->loglik : 0.0);
}

static JSValue dyn_gmm_niter(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->n_iter : 0));
}

static const JSCFunctionListEntry dyn_gmm_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_gmm_fit),
    JS_CFUNC_DEF("predict", 1, dyn_gmm_predict),
    JS_CFUNC_DEF("predictInto", 2, dyn_gmm_predict_into),
    JS_CFUNC_DEF("predictProba", 1, dyn_gmm_predict_proba),
    JS_CGETSET_DEF("weights", dyn_gmm_weights, NULL),
    JS_CGETSET_DEF("means", dyn_gmm_means, NULL),
    JS_CGETSET_DEF("variances", dyn_gmm_variances, NULL),
    JS_CGETSET_DEF("logLikelihood", dyn_gmm_loglik, NULL),
    JS_CGETSET_DEF("nIter", dyn_gmm_niter, NULL),
};

static int dyn_metric_pair(JSContext* ctx, JSValueConst av, JSValueConst bv,
    double** pa, double** pb, size_t* pn)
{
    size_t n;

    if (JS_IsArray(ctx, av)) {
        if (dyn_ml_len(ctx, av, &n))
            return -1;
    } else {
        double* tmp;
        if (dyn_ml_get_f64(ctx, av, &tmp, &n))
            return -1;
    }
    if (n == 0) {
        JS_ThrowRangeError(ctx, "dyna:ml metric needs at least one sample");
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

enum { DYN_METRIC_MSE,
    DYN_METRIC_MAE,
    DYN_METRIC_R2,
    DYN_METRIC_LOGLOSS,
    DYN_METRIC_ACCURACY };

static JSValue dyn_metric_logloss_multi(JSContext* ctx, JSValueConst yv,
    JSValueConst pv)
{
    dyn_matrix_t P = { 0 };
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
    for (i = 0; i < P.rows; i++)
        if (!isfinite(yt[i])) {
            dyn_matrix_free(&P);
            free(yt);
            return JS_ThrowRangeError(ctx, "ml.logLoss: logLoss labels must be finite");
        }
    n_classes = dyn_ml_classes(ctx, yt, P.rows, &classes);
    if (n_classes == 0) {
        dyn_matrix_free(&P);
        free(yt);
        return JS_EXCEPTION;
    }
    if (n_classes != P.cols) {
        dyn_matrix_free(&P);
        free(yt);
        free(classes);
        return JS_ThrowTypeError(ctx,
            "ml.logLoss: yPred has %u columns but yTrue names %u distinct labels; "
            "the columns are the classes in ascending label order",
            (unsigned)P.cols, (unsigned)n_classes);
    }
    for (i = 0; i < P.rows; i++) {
        double p = 0.0;
        for (k = 0; k < n_classes; k++)
            if (classes[k] == yt[i])
                p = P.data[i * P.cols + k];
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

static int dyn_ml_is_matrix(JSContext* ctx, JSValueConst v)
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

static JSValue dyn_metric(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i;
    double acc = 0.0, result;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yPred)");
    if (magic == DYN_METRIC_LOGLOSS && dyn_ml_is_matrix(ctx, argv[1]))
        return dyn_metric_logloss_multi(ctx, argv[0], argv[1]);
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        if (!isfinite(yt[i]) || !isfinite(yp[i])) {
            free(yt);
            free(yp);
            return JS_ThrowRangeError(ctx,
                "dyna:ml metric inputs must be finite");
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
        const double EPS = 1e-15;
        for (i = 0; i < n; i++) {
            double p = yp[i];
            double t = (yt[i] != 0.0) ? 1.0 : 0.0;
            if (p < EPS)
                p = EPS;
            if (p > 1.0 - EPS)
                p = 1.0 - EPS;
            acc += t ? -log(p) : -log(1.0 - p);
        }
        result = acc / (double)n;
        break;
    }
    default:
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

typedef struct {
    double tp, fp, tn, fn_;
} dyn_binconf;

static dyn_binconf dyn_binconf_of(const double* yt, const double* yp, size_t n,
    double pos)
{
    dyn_binconf c = { 0, 0, 0, 0 };
    size_t i;
    for (i = 0; i < n; i++) {
        int t = (yt[i] == pos), p = (yp[i] == pos);
        if (t && p)
            c.tp += 1.0;
        else if (!t && p)
            c.fp += 1.0;
        else if (!t && !p)
            c.tn += 1.0;
        else
            c.fn_ += 1.0;
    }
    return c;
}

enum { DYN_BM_PRECISION,
    DYN_BM_RECALL,
    DYN_BM_F1,
    DYN_BM_SPECIFICITY,
    DYN_BM_BALANCED_ACC,
    DYN_BM_MCC,
    DYN_BM_KAPPA };

static JSValue dyn_metric_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    double *yt = NULL, *yp = NULL, pos = 1.0, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yPred[, positive])");
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt);
    free(yp);

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
        double num = c.tp * c.tn - c.fp * c.fn_;
        double den = sqrt((c.tp + c.fp) * (c.tp + c.fn_) * (c.tn + c.fp) * (c.tn + c.fn_));
        r = (den == 0.0) ? 0.0 : num / den;
        break;
    }
    default: {
        double tot = c.tp + c.fp + c.tn + c.fn_;
        double po, pe;
        if (tot == 0.0) {
            r = 0.0;
            break;
        }
        po = (c.tp + c.tn) / tot;
        pe = ((c.tp + c.fp) * (c.tp + c.fn_) + (c.tn + c.fn_) * (c.tn + c.fp)) / (tot * tot);
        r = (pe == 1.0) ? 0.0 : (po - pe) / (1.0 - pe);
        break;
    }
    }
    return JS_NewFloat64(ctx, r);
}

static JSValue dyn_metric_fbeta(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *yt = NULL, *yp = NULL, b = 1.0, pos = 1.0, p, q, b2, r;
    size_t n;
    dyn_binconf c;

    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "ml.fbeta: expected (yTrue, yPred, beta[, positive])");
    if (JS_ToFloat64(ctx, &b, argv[2]))
        return JS_EXCEPTION;
    if (argc >= 4 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &pos, argv[3]))
        return JS_EXCEPTION;
    if (!(b > 0.0))
        return JS_ThrowRangeError(ctx, "ml.fbeta: beta must be positive");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    c = dyn_binconf_of(yt, yp, n, pos);
    free(yt);
    free(yp);
    p = (c.tp + c.fp) == 0.0 ? 0.0 : c.tp / (c.tp + c.fp);
    q = (c.tp + c.fn_) == 0.0 ? 0.0 : c.tp / (c.tp + c.fn_);
    b2 = b * b;
    r = (b2 * p + q) == 0.0 ? 0.0 : (1.0 + b2) * p * q / (b2 * p + q);
    return JS_NewFloat64(ctx, r);
}

static int dyn_auc_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1
                                  : 0;
}

typedef struct {
    double s;
    size_t i;
} dyn_ap_kv_t;

static int dyn_ap_cmp(const void* a, const void* b)
{
    const dyn_ap_kv_t *x = (const dyn_ap_kv_t*)a, *y = (const dyn_ap_kv_t*)b;
    if (x->s != y->s)
        return (x->s < y->s) ? 1 : -1;
    return (x->i > y->i) ? 1 : (x->i < y->i) ? -1
                                             : 0;
}

static JSValue dyn_metric_auc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_pr)
{
    double *yt = NULL, *ys = NULL, pos = 1.0, r = 0.0;
    size_t n, i;
    double npos = 0.0, nneg = 0.0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ml expected (yTrue, yScore[, positive])");
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &pos, argv[2]))
        return JS_EXCEPTION;
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &ys, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        if (!isfinite(yt[i]) || !isfinite(ys[i])) {
            free(yt);
            free(ys);
            return JS_ThrowRangeError(ctx,
                "dyna:ml rocAuc scores and labels must be finite");
        }
    }
    for (i = 0; i < n; i++) {
        if (yt[i] == pos)
            npos += 1.0;
        else
            nneg += 1.0;
    }
    if (npos == 0.0 || nneg == 0.0) {
        free(yt);
        free(ys);
        return JS_ThrowRangeError(ctx,
            "dyna:ml rocAuc: needs both a positive and a negative sample");
    }

    if (!want_pr) {
        double* sorted = (double*)malloc(n * sizeof(double));
        double ranksum = 0.0;
        if (!sorted) {
            free(yt);
            free(ys);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(sorted, ys, n * sizeof(double));
        dyn_ml_isort(sorted, n, sizeof(double), dyn_auc_cmp);
        for (i = 0; i < n; i++) {
            if (yt[i] != pos)
                continue;
            {
                size_t lo = 0, hi = n, mid, first, last;
                while (lo < hi) {
                    mid = (lo + hi) / 2;
                    if (sorted[mid] < ys[i])
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                first = lo;
                lo = 0;
                hi = n;
                while (lo < hi) {
                    mid = (lo + hi) / 2;
                    if (sorted[mid] <= ys[i])
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                last = lo;
                ranksum += ((double)(first + 1) + (double)last) / 2.0;
            }
        }
        free(sorted);
        r = (ranksum - npos * (npos + 1.0) / 2.0) / (npos * nneg);
    } else {
        dyn_ap_kv_t* kv = (dyn_ap_kv_t*)malloc(n * sizeof(*kv));
        double tp = 0.0, fp = 0.0, ap = 0.0;
        size_t at = 0;
        if (!kv) {
            free(yt);
            free(ys);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++) {
            kv[i].s = ys[i];
            kv[i].i = i;
        }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ap_cmp);
        while (at < n) {
            size_t j = at;
            double tp_end = tp, fp_end = fp;
            do {
                if (yt[kv[j].i] == pos)
                    tp_end += 1.0;
                else
                    fp_end += 1.0;
                j++;
            } while (j < n && kv[j].s == kv[at].s);
            ap += (tp_end - tp) * tp_end / (tp_end + fp_end);
            tp = tp_end;
            fp = fp_end;
            at = j;
        }
        free(kv);
        r = ap / npos;
    }
    free(yt);
    free(ys);
    return JS_NewFloat64(ctx, r);
}

static void dyn_ml_shuffle_idx(uint32_t* idx, size_t n, uint64_t* state)
{
    size_t i;
    for (i = n; i > 1; i--) {
        size_t j = (size_t)(dyn_splitmix64(state) % (uint64_t)i);
        uint32_t t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
}

static JSValue dyn_ml_idx_array(JSContext* ctx, const uint32_t* v, size_t n)
{
    JSValue a = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i,
                JS_NewUint32(ctx, v[i]),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

static JSValue dyn_ml_split_obj(JSContext* ctx, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte)
{
    JSValue o = JS_NewObject(ctx), a;
    if (JS_IsException(o))
        return o;
    a = dyn_ml_idx_array(ctx, tr, ntr);
    if (JS_IsException(a) || JS_DefinePropertyValueStr(ctx, o, "train", a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    a = dyn_ml_idx_array(ctx, te, nte);
    if (JS_IsException(a) || JS_DefinePropertyValueStr(ctx, o, "test", a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int dyn_ml_sel_opts(JSContext* ctx, JSValueConst opt, uint64_t* seed,
    int* shuffle, double* test_size, int32_t* k)
{
    JSValue v;
    if (JS_IsUndefined(opt) || JS_IsNull(opt))
        return 0;
    if (!JS_IsObject(opt)) {
        JS_ThrowTypeError(ctx, "dyna:ml options must be an object");
        return -1;
    }
#define OPT_GET(name, body)                    \
    do {                                       \
        v = JS_GetPropertyStr(ctx, opt, name); \
        if (JS_IsException(v))                 \
            return -1;                         \
        if (!JS_IsUndefined(v)) {              \
            body                               \
        }                                      \
        JS_FreeValue(ctx, v);                  \
    } while (0)

    OPT_GET("seed", { double d; int64_t sd;
                      if (JS_ToFloat64(ctx, &d, v)) { JS_FreeValue(ctx, v); return -1; }
                      if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
                          JS_ThrowRangeError(ctx, "dyna:ml seed must fit the int64 range");
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
        OPT_GET("folds", { if (JS_ToInt32(ctx, k, v)) { JS_FreeValue(ctx, v); return -1; } });
    }
#undef OPT_GET
    return 0;
}

static JSValue dyn_ml_train_test_split(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t seed = 12345;
    int shuffle = 1;
    double test_size = 0.25, nd;
    uint32_t* idx = NULL;
    size_t n, ntest, i;
    JSValue r;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.trainTestSplit(n[, options])");
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_split_keys, 3))
        return JS_EXCEPTION;
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, &test_size, NULL)
        < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "ml.trainTestSplit: needs at least 2 samples");
    if (!(test_size > 0.0 && test_size < 1.0))
        return JS_ThrowRangeError(ctx, "ml.trainTestSplit: testSize must be in (0, 1)");
    n = (size_t)nd;
    ntest = (size_t)floor((double)n * test_size + 0.5);
    if (ntest < 1)
        ntest = 1;
    if (ntest > n - 1)
        ntest = n - 1;

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++)
        idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);
    r = dyn_ml_split_obj(ctx, idx + ntest, n - ntest, idx, ntest);
    free(idx);
    return r;
}

static JSValue dyn_ml_kfold_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
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
        return JS_ThrowTypeError(ctx, "ml.kFold(n[, options])");
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, NULL, &k)
        < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "ml.kFold: needs at least 2 samples");
    n = (size_t)nd;
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "ml.kFold: k must be in [2, n]");
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "ml.kFold: k * n exceeds the output index budget (20000000)");

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    tr = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx || !tr) {
        free(idx);
        free(tr);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++)
        idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        free(idx);
        free(tr);
        return JS_EXCEPTION;
    }
    for (f = 0; f < (size_t)k; f++) {
        size_t sz = n / (size_t)k + (f < n % (size_t)k ? 1 : 0), ntr = 0, j;
        JSValue e;
        for (j = 0; j < n; j++)
            if (j < start || j >= start + sz)
                tr[ntr++] = idx[j];
        e = dyn_ml_split_obj(ctx, tr, ntr, idx + start, sz);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            free(idx);
            free(tr);
            return JS_EXCEPTION;
        }
        start += sz;
    }
    free(idx);
    free(tr);
    return out;
}

static JSValue dyn_ml_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_fold_keys, 4))
        return JS_EXCEPTION;
    return dyn_ml_kfold_impl(ctx, this_val, argc, argv);
}

static JSValue dyn_ml_stratified_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t seed = 12345;
    int shuffle = 0;
    int32_t k = 5;
    double* y = NULL;
    uint32_t *idx = NULL, *fold = NULL, *tr = NULL, *te = NULL;
    size_t n = 0, i, f;
    JSValue out, lv;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.stratifiedKFold(y[, options])");
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_fold_keys, 4))
        return JS_EXCEPTION;
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, NULL, &k)
        < 0)
        return JS_EXCEPTION;
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ml.stratifiedKFold: y must be an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv))
        return JS_EXCEPTION;
    {
        double nd;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (!(nd >= 2.0) || nd > 1e8)
            return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: needs at least 2 samples");
        n = (size_t)nd;
    }
    if (k < 2 || (size_t)k > n)
        return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: k must be in [2, n]");
    if ((size_t)k > 20000000 / n)
        return JS_ThrowRangeError(ctx,
            "ml.stratifiedKFold: k * n exceeds the output index budget (20000000)");
    y = (double*)malloc(n * sizeof(double));
    if (!y)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &y[i], e)) {
            JS_FreeValue(ctx, e);
            free(y);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (!isfinite(y[i])) {
            free(y);
            return JS_ThrowRangeError(ctx, "ml.stratifiedKFold: labels must be finite");
        }
    }

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    fold = (uint32_t*)malloc(n * sizeof(uint32_t));
    tr = (uint32_t*)malloc(n * sizeof(uint32_t));
    te = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx || !fold || !tr || !te) {
        free(y);
        free(idx);
        free(fold);
        free(tr);
        free(te);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        idx[i] = (uint32_t)i;
        fold[i] = 0;
    }
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);

    {
        dyn_tree_kv_t* kv = (dyn_tree_kv_t*)malloc(n * sizeof(*kv));
        size_t run, pos, cnt;
        if (!kv) {
            free(y);
            free(idx);
            free(fold);
            free(tr);
            free(te);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < n; i++) {
            kv[i].key = y[idx[i]];
            kv[i].row = idx[i];
        }
        dyn_ml_isort(kv, n, sizeof(*kv), dyn_ml_kv_cmp);
        run = 0;
        while (run < n) {
            double k0 = kv[run].key;
            uint32_t foldno = 0;
            cnt = 0;
            for (pos = run; pos < n && kv[pos].key == k0; pos++, cnt++) {
                fold[kv[pos].row] = foldno + 1u;
                if (++foldno == (uint32_t)k)
                    foldno = 0;
            }
            if (cnt < (size_t)k) {
                JS_ThrowRangeError(ctx,
                    "ml.stratifiedKFold: a class has %u member(s) but k = %d; "
                    "k must not exceed the smallest class count",
                    (unsigned)cnt, (int)k);
                free(kv);
                goto fail;
            }
            run = pos;
        }
        free(kv);
    }

    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto fail;
    for (f = 0; f < (size_t)k; f++) {
        size_t ntr = 0, nte = 0;
        JSValue e;
        for (i = 0; i < n; i++) {
            if (fold[i] == (uint32_t)f + 1u)
                te[nte++] = (uint32_t)i;
            else
                tr[ntr++] = (uint32_t)i;
        }
        e = dyn_ml_split_obj(ctx, tr, ntr, te, nte);
        if (JS_IsException(e) || JS_DefinePropertyValueUint32(ctx, out, (uint32_t)f, e, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            goto fail;
        }
    }
    free(y);
    free(idx);
    free(fold);
    free(tr);
    free(te);
    return out;
fail:
    free(y);
    free(idx);
    free(fold);
    free(tr);
    free(te);
    return JS_EXCEPTION;
}

static JSValue dyn_metric_confusion(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double *yt = NULL, *yp = NULL;
    size_t n, i, nc;
    double maxlab = 0.0;
    double* cm;
    JSValue out;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ml.confusionMatrix: expected (yTrue, yPred)");
    if (dyn_metric_pair(ctx, argv[0], argv[1], &yt, &yp, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        double a = yt[i], b = yp[i];
        if (!(a >= 0.0) || !(b >= 0.0) || a != floor(a) || b != floor(b)) {
            free(yt);
            free(yp);
            return JS_ThrowRangeError(ctx,
                "ml.confusionMatrix: confusionMatrix labels must be non-negative integers");
        }
        if (a > maxlab)
            maxlab = a;
        if (b > maxlab)
            maxlab = b;
    }
    if (maxlab > 4095.0) {
        free(yt);
        free(yp);
        return JS_ThrowRangeError(ctx,
            "ml.confusionMatrix: confusionMatrix supports labels up to 4095");
    }
    nc = (size_t)maxlab + 1;
    cm = (double*)calloc(nc * nc, sizeof(double));
    if (!cm) {
        free(yt);
        free(yp);
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

static JSValue dyn_ml_rows_subset(JSContext* ctx, const dyn_matrix_t* mx,
    const uint32_t* idx, size_t n)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i, j;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue row = JS_NewArray(ctx);
        const double* src = mx->data + (size_t)idx[i] * mx->cols;
        if (JS_IsException(row)) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        for (j = 0; j < mx->cols; j++)
            if (JS_DefinePropertyValueUint32(ctx, row, (uint32_t)j,
                    JS_NewFloat64(ctx, src[j]), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, row);
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, row,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue dyn_ml_vec_subset(JSContext* ctx, const double* v,
    const uint32_t* idx, size_t n)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewFloat64(ctx, v[idx[i]]), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static int dyn_ml_accuracy_js(JSContext* ctx, JSValueConst yTrue,
    JSValueConst yPred, double* out)
{
    size_t n, m, i, hit = 0;
    if (dyn_ml_len(ctx, yTrue, &n) || dyn_ml_len(ctx, yPred, &m))
        return -1;
    if (n != m) {
        JS_ThrowTypeError(ctx, "ml.scorer: yTrue and yPred have different lengths");
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, yTrue, (uint32_t)i);
        JSValue b = JS_GetPropertyUint32(ctx, yPred, (uint32_t)i);
        double da, db;
        int bad = JS_IsException(a) || JS_IsException(b) || JS_ToFloat64(ctx, &da, a) || JS_ToFloat64(ctx, &db, b);
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

static int dyn_ml_apply_scorer(JSContext* ctx, JSValueConst scorer,
    JSValueConst yTrue, JSValueConst yPred,
    double* out)
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

static int dyn_ml_score_fold(JSContext* ctx, JSValueConst factory,
    JSValueConst scorer, const dyn_matrix_t* mx,
    const double* y, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte, double* out)
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
        JS_ThrowTypeError(ctx, "dyna:ml estimator must be a factory returning a model, "
                               "e.g. () => new LogisticRegression()");
        goto done;
    }
    Xtr = dyn_ml_rows_subset(ctx, mx, tr, ntr);
    ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
    Xte = dyn_ml_rows_subset(ctx, mx, te, nte);
    yte = dyn_ml_vec_subset(ctx, y, te, nte);
    if (JS_IsException(Xtr) || JS_IsException(ytr) || JS_IsException(Xte) || JS_IsException(yte))
        goto done;

    fit = JS_GetPropertyStr(ctx, model, "fit");
    predict = JS_GetPropertyStr(ctx, model, "predict");
    if (JS_IsException(fit) || JS_IsException(predict) || !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
        JS_ThrowTypeError(ctx, "dyna:ml the estimator needs fit() and predict()");
        goto done;
    }
    args[0] = Xtr;
    args[1] = ytr;
    r = JS_Call(ctx, fit, model, 2, args);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);
    r = JS_UNDEFINED;
    pred = JS_Call(ctx, predict, model, 1, (JSValueConst*)&Xte);
    if (JS_IsException(pred))
        goto done;
    if (dyn_ml_apply_scorer(ctx, scorer, yte, pred, out))
        goto done;
    rc = 0;
done:
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
    JS_FreeValue(ctx, model);
    JS_FreeValue(ctx, Xtr);
    JS_FreeValue(ctx, ytr);
    JS_FreeValue(ctx, Xte);
    JS_FreeValue(ctx, yte);
    JS_FreeValue(ctx, pred);
    JS_FreeValue(ctx, fit);
    JS_FreeValue(ctx, predict);
    JS_FreeValue(ctx, r);
    return rc;
}

static int dyn_ml_folds(JSContext* ctx, JSValueConst opts, size_t rows,
    JSValue* out)
{
    JSValue args[2], r;
    args[0] = JS_NewInt64(ctx, (int64_t)rows);
    args[1] = JS_IsObject(opts) ? JS_DupValue(ctx, opts) : JS_UNDEFINED;
    r = dyn_ml_kfold_impl(ctx, JS_UNDEFINED, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r))
        return -1;
    *out = r;
    return 0;
}

static int dyn_ml_read_idx(JSContext* ctx, JSValueConst arr, uint32_t** out,
    size_t* n, size_t rows)
{
    int64_t len;
    size_t i;
    uint32_t* v;
    if (dyn_ml_len(ctx, arr, n))
        return -1;
    len = (int64_t)*n;
    v = (uint32_t*)malloc((*n ? *n : 1) * sizeof(uint32_t));
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
        if (iv < 0 || (uint64_t)iv >= (uint64_t)rows) {
            free(v);
            JS_ThrowRangeError(ctx, "dyna:ml fold index %lld is outside 0..%zu", (long long)iv, rows);
            return -1;
        }
        v[i] = (uint32_t)iv;
    }
    *out = v;
    return 0;
}

static int dyn_ml_search_ingest(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    dyn_matrix_t* mx, double** py)
{
    if (JS_IsArray(ctx, xv))
        return dyn_ml_ingest_Xy_no_w(ctx, xv, yv, JS_UNDEFINED, JS_UNDEFINED,
            mx, py);
    {
        size_t rows, tot, cols;
        double* src;
        JSValue rv, cv;
        int ret;
        if (JS_IsArray(ctx, yv)) {
            if (dyn_ml_len(ctx, yv, &rows))
                return -1;
        } else {
            if (dyn_ml_get_f64(ctx, yv, &src, &rows))
                return -1;
        }
        if (rows == 0) {
            JS_ThrowTypeError(ctx, "dyna:ml y must have at least one entry");
            return -1;
        }
        if (dyn_ml_get_f64(ctx, xv, &src, &tot))
            return -1;
        if (tot % rows != 0 || (cols = tot / rows) == 0) {
            JS_ThrowTypeError(ctx,
                "dyna:ml flat Float64Array X length must equal rows*cols");
            return -1;
        }
        rv = JS_NewInt64(ctx, (int64_t)rows);
        cv = JS_NewInt64(ctx, (int64_t)cols);
        ret = dyn_ml_ingest_Xy_no_w(ctx, xv, yv, rv, cv, mx, py);
        JS_FreeValue(ctx, rv);
        JS_FreeValue(ctx, cv);
        if (ret)
            return -1;
        {
            double* copy = (double*)malloc(mx->rows * mx->cols * sizeof(double));
            if (!copy) {
                dyn_matrix_free(mx);
                free(*py);
                *py = NULL;
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            memcpy(copy, mx->data, mx->rows * mx->cols * sizeof(double));
            mx->data = copy;
            mx->owned = 1;
        }
    }
    return 0;
}

static int dyn_ml_parse_jobs(JSContext* ctx, JSValueConst opts,
    size_t* pnjobs, JSValue* ponprogress)
{
    JSValue v;

    *pnjobs = 1;
    *ponprogress = JS_UNDEFINED;
    if (JS_IsUndefined(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, "nJobs");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int64_t j;
        if (JS_ToInt64(ctx, &j, v)) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (j < 1) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx,
                "dyna:ml nJobs must be at least 1; parallel execution is "
                "reserved (an os.Worker cannot receive the estimator factory, "
                "so folds currently run sequentially)");
            return -1;
        }
        *pnjobs = (size_t)j;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "onProgress");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsFunction(ctx, v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                "dyna:ml onProgress must be a function ({done, total})");
            return -1;
        }
        *ponprogress = v;
    } else {
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static int dyn_ml_progress(JSContext* ctx, JSValueConst onprogress,
    size_t done, size_t total)
{
    JSValue rec, r;

    if (!JS_IsFunction(ctx, onprogress))
        return 0;
    rec = JS_NewObject(ctx);
    if (JS_IsException(rec))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, rec, "done", JS_NewInt64(ctx, (int64_t)done),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, rec, "total", JS_NewInt64(ctx, (int64_t)total),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, rec);
        return -1;
    }
    r = JS_Call(ctx, onprogress, JS_UNDEFINED, 1, (JSValueConst*)&rec);
    JS_FreeValue(ctx, rec);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static JSValue dyn_ml_cross_val_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValue onprogress = JS_UNDEFINED;
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    size_t nfold, f, njobs = 1;
    (void)this_val;

    if (argc < 3 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "ml.crossValScore(estimatorFactory, X, y[, options])");
    if (dyn_opts_strict(ctx, argc > 3 ? argv[3] : JS_UNDEFINED,
            ml_crossval_keys,
            (int)(sizeof(ml_crossval_keys) / sizeof(ml_crossval_keys[0]))))
        return JS_EXCEPTION;
    if (dyn_ml_parse_jobs(ctx, opts, &njobs, &onprogress))
        return JS_EXCEPTION;
    if (dyn_ml_search_ingest(ctx, argv[1], argv[2], &mx, &y))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        scorer = JS_GetPropertyStr(ctx, opts, "scoring");
        if (JS_IsException(scorer))
            goto done;
    }
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds))
        goto done;
    if (dyn_ml_len(ctx, folds, &nfold))
        goto done;
    scores = (double*)malloc((nfold ? nfold : 1) * sizeof(double));
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
        bad = JS_IsException(trv) || JS_IsException(tev) || dyn_ml_read_idx(ctx, trv, &tr, &ntr, mx.rows) || dyn_ml_read_idx(ctx, tev, &te, &nte, mx.rows);
        JS_FreeValue(ctx, trv);
        JS_FreeValue(ctx, tev);
        if (bad || dyn_ml_score_fold(ctx, argv[0], scorer, &mx, y, tr, ntr, te, nte, &scores[f])) {
            free(tr);
            free(te);
            goto done;
        }
        free(tr);
        free(te);
        if (dyn_ml_progress(ctx, onprogress, f + 1, nfold))
            goto done;
    }
    out = dyn_ml_doubles_to_js(ctx, scores, nfold);
done:
    dyn_matrix_free(&mx);
    free(y);
    free(scores);
    JS_FreeValue(ctx, folds);
    JS_FreeValue(ctx, scorer);
    JS_FreeValue(ctx, onprogress);
    return out;
}

static JSValue dyn_ml_grid_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int random)
{
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *scores = NULL;
    JSValue folds = JS_UNDEFINED, scorer = JS_UNDEFINED, out = JS_EXCEPTION;
    JSValue results = JS_UNDEFINED, best = JS_UNDEFINED, keys = JS_UNDEFINED;
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    size_t nfold = 0, nkey = 0, combo, k, f;
    size_t *lens = NULL, *pos = NULL;
    size_t njobs = 1, work = 0;
    JSValue onprogress = JS_UNDEFINED;
    uint64_t seed = 12345;
    int64_t n_iter = 10;
    double bestScore = -1e308;
    uint64_t total = 1;
    (void)this_val;

    if (argc < 4 || !JS_IsFunction(ctx, argv[0]) || !JS_IsObject(argv[3]))
        return JS_ThrowTypeError(ctx, random ? "ml.randomSearch(estimatorFactory, X, y, grid[, options])" : "ml.gridSearch(estimatorFactory, X, y, grid[, options])");
    if (dyn_opts_strict(ctx, argc > 4 ? argv[4] : JS_UNDEFINED,
            ml_search_keys,
            (int)(sizeof(ml_search_keys) / sizeof(ml_search_keys[0]))))
        return JS_EXCEPTION;
    if (dyn_ml_parse_jobs(ctx, opts, &njobs, &onprogress))
        return JS_EXCEPTION;
    if (dyn_ml_search_ingest(ctx, argv[1], argv[2], &mx, &y))
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
        JS_ThrowRangeError(ctx, "dyna:ml nIter must be at least 1");
        goto done;
    }
    if (dyn_ml_folds(ctx, opts, mx.rows, &folds) || dyn_ml_len(ctx, folds, &nfold))
        goto done;

    {
        JSPropertyEnum* tab;
        uint32_t cnt, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, argv[3],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto done;
        keys = JS_NewArray(ctx);
        if (JS_IsException(keys)) {
            for (i = 0; i < cnt; i++)
                JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            goto done;
        }
        for (i = 0; i < cnt; i++) {
            JSValue kn = JS_AtomToString(ctx, tab[i].atom);
            JS_FreeAtom(ctx, tab[i].atom);
            if (JS_IsException(kn) || JS_DefinePropertyValueUint32(ctx, keys, i, kn, JS_PROP_C_W_E) < 0) {
                for (++i; i < cnt; i++)
                    JS_FreeAtom(ctx, tab[i].atom);
                js_free(ctx, tab);
                goto done;
            }
        }
        js_free(ctx, tab);
        nkey = cnt;
    }
    if (nkey == 0) {
        JS_ThrowTypeError(ctx, "dyna:ml the grid has no parameters");
        goto done;
    }
    lens = (size_t*)malloc(nkey * sizeof(size_t));
    pos = (size_t*)malloc(nkey * sizeof(size_t));
    scores = (double*)malloc((nfold ? nfold : 1) * sizeof(double));
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
        if (JS_IsException(vals) || !JS_IsArray(ctx, vals) || dyn_ml_len(ctx, vals, &lens[k])) {
            JS_FreeValue(ctx, vals);
            JS_ThrowTypeError(ctx, "dyna:ml every grid value must be an array");
            goto done;
        }
        JS_FreeValue(ctx, vals);
        if (lens[k] == 0) {
            JS_ThrowTypeError(ctx, "dyna:ml a grid parameter has no values");
            goto done;
        }
        if (total > (size_t)1 << 20 || lens[k] > (size_t)1 << 20
            || (uint64_t)total * lens[k] > (uint64_t)1 << 20) {
            JS_ThrowRangeError(ctx, "dyna:ml the grid has more than 2^20 points");
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
            const char* kname = JS_ToCString(ctx, kn);
            JSValue vals, one;
            size_t pick;
            JS_FreeValue(ctx, kn);
            if (!kname) {
                JS_FreeValue(ctx, params);
                goto done;
            }
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
            if (JS_IsException(one) || JS_DefinePropertyValueStr(ctx, params, kname, one, JS_PROP_C_W_E) < 0) {
                JS_FreeCString(ctx, kname);
                JS_FreeValue(ctx, params);
                goto done;
            }
            JS_FreeCString(ctx, kname);
        }
        for (f = 0; f < nfold; f++) {
            JSValue fold = JS_GetPropertyUint32(ctx, folds, (uint32_t)f);
            JSValue trv, tev;
            uint32_t *tr = NULL, *te = NULL;
            size_t ntr = 0, nte = 0;
            int bad;
            if (JS_IsException(fold)) {
                JS_FreeValue(ctx, params);
                goto done;
            }
            trv = JS_GetPropertyStr(ctx, fold, "train");
            tev = JS_GetPropertyStr(ctx, fold, "test");
            JS_FreeValue(ctx, fold);
            bad = JS_IsException(trv) || JS_IsException(tev) || dyn_ml_read_idx(ctx, trv, &tr, &ntr, mx.rows) || dyn_ml_read_idx(ctx, tev, &te, &nte, mx.rows);
            JS_FreeValue(ctx, trv);
            JS_FreeValue(ctx, tev);
            if (bad) {
                free(tr);
                free(te);
                JS_FreeValue(ctx, params);
                goto done;
            }
            {
                JSValue m = JS_Call(ctx, argv[0], JS_UNDEFINED, 1,
                    (JSValueConst*)&params);
                JSValue fit, predict, Xtr, ytr, Xte, yte, pred, r;
                JSValueConst a2[2];
                int ok = 0;
                if (JS_IsException(m)) {
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                if (!JS_IsObject(m)) {
                    JS_FreeValue(ctx, m);
                    JS_ThrowTypeError(ctx,
                        "dyna:ml estimator must be a factory returning a model, "
                        "e.g. () => new LogisticRegression()");
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                Xtr = dyn_ml_rows_subset(ctx, &mx, tr, ntr);
                ytr = dyn_ml_vec_subset(ctx, y, tr, ntr);
                Xte = dyn_ml_rows_subset(ctx, &mx, te, nte);
                yte = dyn_ml_vec_subset(ctx, y, te, nte);
                if (JS_IsException(Xtr) || JS_IsException(ytr) || JS_IsException(Xte) || JS_IsException(yte)) {
                    JS_FreeValue(ctx, m);
                    JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr);
                    JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                fit = JS_GetPropertyStr(ctx, m, "fit");
                predict = JS_GetPropertyStr(ctx, m, "predict");
                if (JS_IsException(fit) || JS_IsException(predict) || !JS_IsFunction(ctx, fit) || !JS_IsFunction(ctx, predict)) {
                    JS_FreeValue(ctx, fit);
                    JS_FreeValue(ctx, predict);
                    JS_FreeValue(ctx, m);
                    JS_FreeValue(ctx, Xtr);
                    JS_FreeValue(ctx, ytr);
                    JS_FreeValue(ctx, Xte);
                    JS_FreeValue(ctx, yte);
                    JS_ThrowTypeError(ctx, "dyna:ml the estimator needs fit() and predict()");
                    free(tr);
                    free(te);
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                a2[0] = Xtr;
                a2[1] = ytr;
                r = JS_Call(ctx, fit, m, 2, a2);
                pred = JS_IsException(r) ? JS_EXCEPTION
                                         : JS_Call(ctx, predict, m, 1, (JSValueConst*)&Xte);
                JS_FreeValue(ctx, r);
                if (!JS_IsException(pred) && !dyn_ml_apply_scorer(ctx, scorer, yte, pred, &scores[f]))
                    ok = 1;
                JS_FreeValue(ctx, pred);
                JS_FreeValue(ctx, fit);
                JS_FreeValue(ctx, predict);
                JS_FreeValue(ctx, Xtr);
                JS_FreeValue(ctx, ytr);
                JS_FreeValue(ctx, Xte);
                JS_FreeValue(ctx, yte);
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
                free(tr);
                free(te);
                if (!ok) {
                    JS_FreeValue(ctx, params);
                    goto done;
                }
                if (dyn_ml_progress(ctx, onprogress, ++work,
                        (size_t)total * nfold)) {
                    JS_FreeValue(ctx, params);
                    goto done;
                }
            }
            mean += scores[f];
        }
        mean /= (double)(nfold ? nfold : 1);
        rec = JS_NewObject(ctx);
        sarr = dyn_ml_doubles_to_js(ctx, scores, nfold);
        if (JS_IsException(rec) || JS_IsException(sarr) || JS_DefinePropertyValueStr(ctx, rec, "params", JS_DupValue(ctx, params), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, rec, "scores", sarr, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, rec, "mean", JS_NewFloat64(ctx, mean), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, results, (uint32_t)combo, rec, JS_PROP_C_W_E) < 0) {
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
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, out, "bestScore",
               JS_NewFloat64(ctx, bestScore), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, out, "results", JS_DupValue(ctx, results),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, out);
        out = JS_EXCEPTION;
    }
done:
    dyn_matrix_free(&mx);
    free(y);
    free(scores);
    free(lens);
    free(pos);
    JS_FreeValue(ctx, folds);
    JS_FreeValue(ctx, scorer);
    JS_FreeValue(ctx, results);
    JS_FreeValue(ctx, best);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, onprogress);
    return out;
}

static JSValue dyn_ml_impute_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matrix_t mx = { 0 };
    size_t rows, cols, i, j;
    JSValue out;
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    (void)this_val;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    rows = mx.rows;
    cols = mx.cols;
    if (!mx.owned) {
        double* copy = (double*)malloc(rows * cols * sizeof(double));
        if (!copy) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(copy, mx.data, rows * cols * sizeof(double));
        mx.data = copy;
        mx.owned = 1;
    }
    {
        double* csum = (double*)calloc(cols ? cols : 1, sizeof(double));
        size_t* cn = (size_t*)calloc(cols ? cols : 1, sizeof(size_t));
        if (!csum || !cn) {
            free(csum);
            free(cn);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < rows; i++) {
            const double* ri = mx.data + i * cols;
            for (j = 0; j < cols; j++)
                if (isfinite(ri[j])) {
                    csum[j] += ri[j];
                    cn[j]++;
                }
        }
        for (j = 0; j < cols; j++) {
            if (cn[j] == 0) {
                free(csum);
                free(cn);
                dyn_matrix_free(&mx);
                return JS_ThrowRangeError(ctx,
                    "ml.imputeMean: column %u has no finite value to take a mean from",
                    (unsigned)j);
            }
            csum[j] /= (double)cn[j];
        }
        for (i = 0; i < rows; i++) {
            double* ri = mx.data + i * cols;
            for (j = 0; j < cols; j++)
                if (!isfinite(ri[j]))
                    ri[j] = csum[j];
        }
        free(csum);
        free(cn);
    }
    out = dyn_ml_matrix_to_js(ctx, mx.data, rows, cols, 0);
    dyn_matrix_free(&mx);
    return out;
}

static JSValue dyn_ml_drop_missing(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_matrix_t mx = { 0 };
    double *y = NULL, *outy = NULL;
    uint32_t* keep = NULL;
    size_t rows, cols, i, j, kept = 0;
    JSValue res = JS_EXCEPTION, xs, ys, ks;
    int have_y = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
    (void)this_val;

    if (dyn_ml_ingest_X(ctx, argv[0], argc > 2 ? argv[2] : JS_UNDEFINED,
            argc > 3 ? argv[3] : JS_UNDEFINED, &mx))
        return JS_EXCEPTION;
    rows = mx.rows;
    cols = mx.cols;
    if (have_y && dyn_ml_ingest_vector(ctx, argv[1], rows, &y)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    keep = (uint32_t*)malloc((rows ? rows : 1) * sizeof(uint32_t));
    outy = (double*)malloc((rows ? rows : 1) * sizeof(double));
    if (!keep || !outy) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    {
        double* packed = (double*)malloc((rows ? rows : 1) * cols * sizeof(double));
        if (!packed) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        for (i = 0; i < rows; i++) {
            int ok = 1;
            for (j = 0; j < cols; j++)
                if (!isfinite(mx.data[i * cols + j])) {
                    ok = 0;
                    break;
                }
            if (ok && y && !isfinite(y[i]))
                ok = 0;
            if (!ok)
                continue;
            memcpy(packed + kept * cols, mx.data + i * cols, cols * sizeof(double));
            if (y)
                outy[kept] = y[i];
            keep[kept] = (uint32_t)i;
            kept++;
        }
        res = JS_NewObject(ctx);
        if (JS_IsException(res)) {
            free(packed);
            goto done;
        }
        xs = dyn_ml_matrix_to_js(ctx, packed, kept, cols, 0);
        free(packed);
        if (JS_IsException(xs)) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        if (JS_DefinePropertyValueStr(ctx, res, "X", xs, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        ys = have_y ? dyn_ml_f64array(ctx, outy, kept) : JS_UNDEFINED;
        if (JS_IsException(ys) || JS_DefinePropertyValueStr(ctx, res, "y", ys, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        ks = JS_NewArray(ctx);
        if (JS_IsException(ks)) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
        }
        for (i = 0; i < kept; i++)
            if (JS_DefinePropertyValueUint32(ctx, ks, (uint32_t)i,
                    JS_NewUint32(ctx, keep[i]), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, ks);
                JS_FreeValue(ctx, res);
                res = JS_EXCEPTION;
                goto done;
            }
        if (JS_DefinePropertyValueStr(ctx, res, "kept", ks, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            res = JS_EXCEPTION;
            goto done;
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

#include "dyna-ml-persist.inc.c"

typedef struct {
    JSValue* stage;
    size_t n;
    int fitted;
    JSRuntime* rt;
} dyn_pipe_t;

static JSClassID dyn_pipe_class_id;

static void dyn_pipe_dispose(void* native)
{
    dyn_pipe_t* pp = (dyn_pipe_t*)native;
    size_t i;
    if (!pp)
        return;
    for (i = 0; i < pp->n; i++)
        JS_FreeValueRT(pp->rt, pp->stage[i]);
    free(pp->stage);
    free(pp);
}

static void dyn_pipe_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_pipe_class_id);
    dyn_pipe_t* pp = r ? (dyn_pipe_t*)r->native : NULL;
    size_t i;
    if (!pp || !pp->stage)
        return;
    for (i = 0; i < pp->n; i++)
        JS_MarkValue(rt, pp->stage[i], mark_func);
}

static const JSClassDef dyn_pipe_class = {
    "Pipeline",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_pipe_mark,
};

static JSValue dyn_pipe_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp;
    int64_t len = 0, i;
    JSValue ret;

    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "ml.Pipeline: new Pipeline(stages[]) requires an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (len < 1)
        return JS_ThrowRangeError(ctx, "ml.Pipeline: a Pipeline needs at least one stage");
    if (len > 64)
        return JS_ThrowRangeError(ctx, "ml.Pipeline: at most 64 stages");

    pp = (dyn_pipe_t*)calloc(1, sizeof(*pp));
    if (!pp)
        return JS_ThrowOutOfMemory(ctx);
    pp->rt = JS_GetRuntime(ctx);
    pp->stage = (JSValue*)calloc((size_t)len, sizeof(JSValue));
    if (!pp->stage) {
        free(pp);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < len; i++) {
        JSValue st = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        JSValue fn;
        if (JS_IsException(st))
            goto fail;
        if (!JS_IsObject(st)) {
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "ml.Pipeline: Pipeline stage %d is not an object", (int)i);
            goto fail;
        }
        fn = JS_GetPropertyStr(ctx, st, "fit");
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, st);
            JS_ThrowTypeError(ctx, "ml.Pipeline: Pipeline stage %d has no fit()", (int)i);
            goto fail;
        }
        JS_FreeValue(ctx, fn);
        if (i < len - 1) {
            fn = JS_GetPropertyStr(ctx, st, "transform");
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, st);
                JS_ThrowTypeError(ctx,
                    "ml.Pipeline: Pipeline stage %d has no transform(); only the LAST stage "
                    "may be a bare estimator",
                    (int)i);
                goto fail;
            }
            JS_FreeValue(ctx, fn);
        }
        pp->stage[i] = st;
        pp->n = (size_t)(i + 1);
    }
    ret = dyn_res_wrap(ctx, new_target, dyn_pipe_class_id, pp, dyn_pipe_dispose);
    if (JS_IsException(ret))
        return ret;
    return ret;

fail:
    for (i = 0; i < (int64_t)pp->n; i++)
        JS_FreeValue(ctx, pp->stage[i]);
    free(pp->stage);
    free(pp);
    return JS_EXCEPTION;
}

static JSValue dyn_pipe_call(JSContext* ctx, JSValueConst obj, const char* name,
    int n, JSValueConst* args)
{
    JSValue fn = JS_GetPropertyStr(ctx, obj, name);
    JSValue r;
    if (JS_IsException(fn))
        return fn;
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return JS_ThrowTypeError(ctx, "ml.Pipeline: a stage has no %s()", name);
    }
    r = JS_Call(ctx, fn, obj, n, args);
    JS_FreeValue(ctx, fn);
    return r;
}

static dyn_pipe_t* dyn_pipe_of(JSContext* ctx, JSValueConst t)
{
    DynResource* r = dyn_res_get(ctx, t, dyn_pipe_class_id);
    if (!r)
        return NULL;
    dyn_res_hold(ctx, r);
    return (dyn_pipe_t*)r->native;
}

static JSValue dyn_pipe_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.fit: fit(X, y?) requires X");
    if (dyn_ml_reject_weights(ctx, argc, argv, 2, "Pipeline.fit (set it on the estimator)"))
        return JS_EXCEPTION;

    cur = JS_DupValue(ctx, argv[0]);
    for (i = 0; i + 1 < pp->n; i++) {
        JSValueConst a[1];
        JSValue next;
        a[0] = cur;
        next = dyn_pipe_call(ctx, pp->stage[i], "fit", 1, a);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, cur);
            return next;
        }
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
        if (argc > 1) {
            a[1] = argv[1];
            n = 2;
        }
        r = dyn_pipe_call(ctx, pp->stage[pp->n - 1], "fit", n, a);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    pp->fitted = 1;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_pipe_apply(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    JSValue cur;
    size_t i, upto;

    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:ml X is required");
    if (!pp->fitted)
        return JS_ThrowInternalError(ctx, "ml.Pipeline: predict before fit");

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

static JSValue dyn_pipe_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    if (!pp)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)pp->n);
    if (magic == 1)
        return JS_NewBool(ctx, pp->fitted);
    return JS_DupValue(ctx, pp->stage[pp->n - 1]);
}

static JSValue dyn_pipe_stage(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_pipe_t* pp = dyn_pipe_of(ctx, this_val);
    int64_t i;
    if (!pp)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToInt64(ctx, &i, argv[0]))
        return JS_ThrowTypeError(ctx, "ml.stage: stage(i) requires an index");
    if (i < 0)
        i += (int64_t)pp->n;
    if (i < 0 || i >= (int64_t)pp->n)
        return JS_ThrowRangeError(ctx, "ml.stage: stage index out of range");
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

static int dyn_ml_init_module(JSContext* ctx, JSModuleDef* m)
{
    {
        JSValue ta, zero = JS_NewInt32(ctx, 0);
        JSValueConst ta_args[1];
        ta_args[0] = zero;
        ta = JS_NewTypedArray(ctx, 1, ta_args, JS_TYPED_ARRAY_FLOAT64);
        JS_FreeValue(ctx, zero);
        if (JS_IsException(ta))
            return -1;
        dyn_ml_f64_class_id = JS_GetClassID(ta);
        JS_FreeValue(ctx, ta);
    }
    if (dyn_register_class(ctx, m, &dyn_csr_class_id, &dyn_csr_class,
            dyn_csr_proto, countof(dyn_csr_proto),
            dyn_csr_ctor, "CSR")
        < 0)
        return -1;
    {
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
            dyn_linreg_ctor, "LinearRegression")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_logreg_class_id, &dyn_logreg_class,
            dyn_logreg_proto, countof(dyn_logreg_proto),
            dyn_logreg_ctor, "LogisticRegression")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_kmeans_class_id, &dyn_kmeans_class,
            dyn_kmeans_proto, countof(dyn_kmeans_proto),
            dyn_kmeans_ctor, "KMeans")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_svm_class_id, &dyn_svm_class,
            dyn_svm_proto, countof(dyn_svm_proto),
            dyn_svm_ctor, "SVC")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gmm_class_id, &dyn_gmm_class,
            dyn_gmm_proto, countof(dyn_gmm_proto),
            dyn_gmm_ctor, "GaussianMixture")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtc_class_id, &dyn_dtc_class,
            dyn_dtc_proto, countof(dyn_dtc_proto),
            dyn_dtc_ctor, "DecisionTreeClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtr_class_id, &dyn_dtr_class,
            dyn_dtr_proto, countof(dyn_dtr_proto),
            dyn_dtr_ctor, "DecisionTreeRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfc_class_id, &dyn_rfc_class,
            dyn_rfc_proto, countof(dyn_rfc_proto),
            dyn_rfc_ctor, "RandomForestClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfr_class_id, &dyn_rfr_class,
            dyn_rfr_proto, countof(dyn_rfr_proto),
            dyn_rfr_ctor, "RandomForestRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbr_class_id, &dyn_gbr_class,
            dyn_gbr_proto, countof(dyn_gbr_proto),
            dyn_gbr_ctor, "GradientBoostingRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbc_class_id, &dyn_gbc_class,
            dyn_gbc_proto, countof(dyn_gbc_proto),
            dyn_gbc_ctor, "GradientBoostingClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbr_class_id, &dyn_xgbr_class,
            dyn_xgbr_xproto, countof(dyn_xgbr_xproto),
            dyn_xgbr_ctor, "XGBRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbc_class_id, &dyn_xgbc_class,
            dyn_xgbc_xproto, countof(dyn_xgbc_xproto),
            dyn_xgbc_ctor, "XGBClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pipe_class_id, &dyn_pipe_class,
            dyn_pipe_proto, countof(dyn_pipe_proto),
            dyn_pipe_ctor, "Pipeline")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pca_class_id, &dyn_pca_class,
            dyn_pca_proto, countof(dyn_pca_proto),
            dyn_pca_ctor, "PCA")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_nb_class_id, &dyn_nb_class,
            dyn_nb_proto, countof(dyn_nb_proto),
            dyn_nb_ctor, "GaussianNB")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_clf_class_id, &dyn_knn_clf_class,
            dyn_knn_clf_proto, countof(dyn_knn_clf_proto),
            dyn_knn_clf_ctor, "KNClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_reg_class_id, &dyn_knn_reg_class,
            dyn_knn_reg_proto, countof(dyn_knn_reg_proto),
            dyn_knn_reg_ctor, "KNRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dbscan_class_id, &dyn_dbscan_class,
            dyn_dbscan_proto, countof(dyn_dbscan_proto),
            dyn_dbscan_ctor, "DBScan")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_stdscaler_class_id,
            &dyn_stdscaler_class, dyn_stdscaler_proto,
            countof(dyn_stdscaler_proto),
            dyn_stdscaler_ctor, "StandardScaler")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_minmax_class_id, &dyn_minmax_class,
            dyn_minmax_proto, countof(dyn_minmax_proto),
            dyn_minmax_ctor, "MinMaxScaler")
        < 0)
        return -1;
    if (dyn_ml_install_persistence(ctx) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_ml_funcs, countof(dyn_ml_funcs));
}

int js_nat_init_ml(JSContext* ctx)
{
    JSModuleDef* m;
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

#endif
