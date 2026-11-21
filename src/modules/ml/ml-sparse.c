#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static int dyn_ml_any_at(JSContext* ctx, JSValueConst v, size_t i, double* out)
{
    if (JS_IsArray(ctx, v)) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int bad = JS_IsException(e) || JS_ToFloat64(ctx, out, e);
        JS_FreeValue(ctx, e);
        return bad ? -1 : 0;
    } else {
        double* p;
        size_t n;
        if (dyn_ml_get_f64(ctx, v, &p, &n))
            return -1;
        *out = i < n ? p[i] : DYN_NAN;
        return 0;
    }
}

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

static int dyn_ml_idx_len(JSContext* ctx, JSValueConst v, size_t* pn)
{
    const uint8_t* p;
    int sg, k = dyn_ml_int_view(ctx, v, &p, pn, &sg);
    if (k)
        return k < 0 ? -1 : 0;
    return dyn_ml_any_len(ctx, v, pn);
}

static int dyn_ml_idx_at(JSContext* ctx, JSValueConst v, size_t i, double* out)
{
    const uint8_t* p;
    size_t n;
    int sg, k = dyn_ml_int_view(ctx, v, &p, &n, &sg);
    if (k < 0)
        return -1;
    if (k) {
        *out = i < n ? dyn_ml_int_view_at(p, i, sg) : DYN_NAN;
        return 0;
    }
    return dyn_ml_any_at(ctx, v, i, out);
}

JSValue dyn_csr_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_csr_t* s;
    double *val = NULL, *colf = NULL, *ptrf = NULL;
    int64_t cols64 = 0;
    size_t nnz, ncol, nptr, i;

    if (argc < 4)
        return JS_ThrowTypeError(ctx,
            "ml.CSR: new CSR(values, columns, rowPointers, cols) requires four arguments");
    if (dyn_ml_any_len(ctx, argv[0], &nnz) || dyn_ml_idx_len(ctx, argv[1], &ncol))
        return JS_EXCEPTION;
    if (ncol != nnz)
        return JS_ThrowTypeError(ctx,
            "ml.CSR: columns has %u entries but values has %u; there is one column "
            "index per value",
            (unsigned)ncol, (unsigned)nnz);
    if (dyn_ml_ingest_any_vector(ctx, argv[0], &val, &nnz))
        return JS_EXCEPTION;
    if (dyn_ml_ingest_idx_vector(ctx, argv[1], &colf, &ncol)) {
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
    {
        double first = -1.0, last = -1.0;
        if (dyn_ml_idx_len(ctx, argv[2], &nptr) || (nptr >= 1 && (dyn_ml_idx_at(ctx, argv[2], 0, &first) || dyn_ml_idx_at(ctx, argv[2], nptr - 1, &last)))) {
            free(val);
            free(colf);
            return JS_EXCEPTION;
        }
        if (nptr >= 1 && (first != 0.0 || last != (double)nnz)) {
            free(val);
            free(colf);
            return JS_ThrowRangeError(ctx,
                "ml.CSR: rowPointers must start at 0 and end at the value count (%u)",
                (unsigned)nnz);
        }
    }
    if (dyn_ml_ingest_idx_vector(ctx, argv[2], &ptrf, &nptr)) {
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

JSValue dyn_csr_from_dense(JSContext* ctx, JSValueConst this_val,
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
    dyn_fiber_offload();
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

JSValue dyn_csr_to_dense(JSContext* ctx, JSValueConst this_val,
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
    dyn_fiber_offload();
    for (i = 0; i < s->rows; i++)
        for (k = s->ptr[i]; k < s->ptr[i + 1]; k++)
            out[i * s->cols + s->col[k]] += s->val[k];
    r = dyn_ml_matrix_to_js(ctx, out, s->rows, s->cols, 0);
    free(out);
    return r;
}

JSValue dyn_csr_row(JSContext* ctx, JSValueConst this_val,
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

#endif
