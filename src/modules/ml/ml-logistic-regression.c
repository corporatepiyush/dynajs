#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define DYN_LOGREG_LR 0.1
#define DYN_LOGREG_ITERS 3000

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

static const char* const ml_logreg_keys[] = { "learningRate", "tol", "l1",
    "l2", "maxIter", "C", "penalty",
    "classWeight" };

JSValue dyn_logreg_ctor(JSContext* ctx, JSValueConst new_target,
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

JSValue dyn_logreg_fit(JSContext* ctx, JSValueConst this_val,
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
        dyn_fiber_offload();
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
    dyn_fiber_offload();
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
        return dyn_ml_throw_unfitted(ctx, this_val, proba ? "predictProba" : "predict");
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
    dyn_fiber_offload();
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

JSValue dyn_logreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0, 0);
}

JSValue dyn_logreg_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 1, 0);
}

JSValue dyn_logreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_logreg_predict_impl(ctx, this_val, argc, argv, 0, 1);
}

JSValue dyn_logreg_get_classes(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return dyn_ml_throw_unfitted(ctx, t, "classes");
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

JSValue dyn_logreg_get_niter(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->n_iter);
}

JSValue dyn_logreg_get_converged(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, m->converged);
}

JSValue dyn_logreg_get_coef(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return dyn_ml_throw_unfitted(ctx, t, "coef");
    return dyn_ml_matrix_to_js(ctx, m->coef, m->n_out, m->n_features, 0);
}

JSValue dyn_logreg_get_intercept(JSContext* ctx, JSValueConst t)
{
    dyn_logreg_t* m = (dyn_logreg_t*)dyn_res_native(ctx, t, dyn_logreg_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return dyn_ml_throw_unfitted(ctx, t, "intercept");
    return m->n_out == 1 ? JS_NewFloat64(ctx, m->intercept[0])
                         : dyn_ml_doubles_to_js(ctx, m->intercept, m->n_out);
}

#endif
