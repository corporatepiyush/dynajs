#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define DYN_LINREG_MAX_FEATURES 4096
#define DYN_RIDGE 1e-9
#define DYN_SOLVE_MIN_PIVOT (1000.0 * 2.220446049250313e-16)

JSValue dyn_linreg_ctor(JSContext* ctx, JSValueConst new_target,
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

JSValue dyn_linreg_fit(JSContext* ctx, JSValueConst this_val,
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
        dyn_fiber_offload();
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
    dyn_fiber_offload();
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

JSValue dyn_linreg_coef_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_linreg_t* m = (dyn_linreg_t*)dyn_res_native(ctx, this_val,
        dyn_linreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->coef, m->n_features);
}

JSValue dyn_linreg_intercept_get(JSContext* ctx, JSValueConst this_val)
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
        return dyn_ml_throw_unfitted(ctx, this_val, "predict");
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
    dyn_fiber_offload();
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

JSValue dyn_linreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_linreg_predict_impl(ctx, this_val, argc, argv, 0);
}

JSValue dyn_linreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_linreg_predict_impl(ctx, this_val, argc, argv, 1);
}

#endif
