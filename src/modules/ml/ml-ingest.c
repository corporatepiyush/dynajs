#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define JS_GetFastArray(...) (dyn_fiber_onjs(), JS_GetFastArray(__VA_ARGS__))

void dyn_matrix_free(dyn_matrix_t* mx)
{
    if (mx->owned)
        free(mx->data);
    mx->data = NULL;
}

static int dyn_ml_read_row(JSContext* ctx, JSValueConst arr, double* out,
    size_t n)
{
    size_t j;
    for (j = 0; j < n; j++) {
        double x;
        JSValue* fv;
        uint32_t fc;
        JSValue v;
        if (JS_GetFastArray(ctx, arr, &fv, &fc) && j < fc) {
            int tag = JS_VALUE_GET_TAG(fv[j]);
            if (tag == JS_TAG_INT) {
                out[j] = (double)JS_VALUE_GET_INT(fv[j]);
                continue;
            }
            if (JS_TAG_IS_FLOAT64(tag)) {
                out[j] = JS_VALUE_GET_FLOAT64(fv[j]);
                continue;
            }
        }
        v = JS_GetPropertyUint32(ctx, arr, (uint32_t)j);
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
    JSValue* fv;
    uint32_t fc;
    if (JS_GetFastArray(ctx, row, &fv, &fc) && fc == cols)
        return dyn_ml_read_row(ctx, row, dst, cols);
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

int dyn_ml_ingest_matrix_array(JSContext* ctx, JSValueConst x,
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
    mx->rows = rows;
    mx->cols = cols;
    mx->flat = 1;
    if (dyn_ml_job()) {
        double* copy = (double*)malloc(total * sizeof(double));
        if (!copy) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(copy, data, total * sizeof(double));
        mx->data = copy;
        mx->owned = 1;
        return 0;
    }
    mx->data = data;
    mx->owned = 0;
    return 0;
}

int dyn_ml_ingest_vector(JSContext* ctx, JSValueConst y, size_t expect,
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

int dyn_ml_ingest_any_vector(JSContext* ctx, JSValueConst v,
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

int dyn_ml_check_finite(JSContext* ctx, const dyn_matrix_t* mx,
    const double* y)
{
    return dyn_ml_check_finite_ex(ctx, mx, y, 0);
}

int dyn_ml_check_finite_fast(JSContext* ctx, const dyn_matrix_t* mx)
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

int dyn_ml_ingest_weights(JSContext* ctx, int argc, JSValueConst* argv,
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

int dyn_ml_ingest_Xy_ex(JSContext* ctx, JSValueConst xv, JSValueConst yv,
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

int dyn_ml_ingest_Xy(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** py, double** pw)
{
    return dyn_ml_ingest_Xy_ex(ctx, xv, yv, rows_arg, cols_arg,
        argc, argv, opt_from, mx, py, pw, 0);
}

int dyn_ml_ingest_Xy_no_w(JSContext* ctx, JSValueConst xv,
    JSValueConst yv, JSValueConst rows_arg,
    JSValueConst cols_arg, dyn_matrix_t* mx,
    double** py)
{
    return dyn_ml_ingest_Xy_ex(ctx, xv, yv, rows_arg, cols_arg,
        0, NULL, 0, mx, py, NULL, 0);
}

int dyn_ml_ingest_X(JSContext* ctx, JSValueConst xv,
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

int dyn_ml_ingest_X_w(JSContext* ctx, JSValueConst xv,
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

JSValue dyn_ml_matrix_to_js(JSContext* ctx, const double* v, size_t rows,
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

int dyn_ml_ingest_idx_vector(JSContext* ctx, JSValueConst v, double** pout, size_t* pn)
{
    const uint8_t* p;
    size_t n, i;
    double* out;
    int sg, k = dyn_ml_int_view(ctx, v, &p, &n, &sg);
    if (k < 0)
        return -1;
    if (!k)
        return dyn_ml_ingest_any_vector(ctx, v, pout, pn);
    if (n > SIZE_MAX / sizeof(double)) {
        JS_ThrowRangeError(ctx, "dyna:ml vector length out of range");
        return -1;
    }
    out = (double*)malloc((n ? n : 1) * sizeof(double));
    if (!out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++)
        out[i] = dyn_ml_int_view_at(p, i, sg);
    *pout = out;
    *pn = n;
    return 0;
}

int dyn_ml_check_csr_finite(JSContext* ctx, const dyn_csr_t* S,
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

int dyn_ml_row_src_init(JSContext* ctx, dyn_ml_row_src* rs,
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

const double* dyn_ml_row_at(const dyn_ml_row_src* rs, size_t i)
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

void dyn_ml_row_src_free(dyn_ml_row_src* rs)
{
    free(rs->scratch);
    rs->scratch = NULL;
}

int dyn_ml_check_csr_values_finite(JSContext* ctx, const dyn_csr_t* S)
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

int dyn_ml_is_matrix(JSContext* ctx, JSValueConst v)
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

JSValue dyn_ml_rows_subset(JSContext* ctx, const dyn_matrix_t* mx,
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

int dyn_ml_read_idx(JSContext* ctx, JSValueConst arr, uint32_t** out,
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

JSValue dyn_ml_drop_missing(JSContext* ctx, JSValueConst this_val,
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
        dyn_fiber_offload();
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

#endif
