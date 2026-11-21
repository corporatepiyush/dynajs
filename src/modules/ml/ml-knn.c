#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

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

JSValue dyn_knn_clf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return dyn_knn_new(ctx, new_target, argc, argv, dyn_knn_clf_class_id, 0);
}

JSValue dyn_knn_reg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return dyn_knn_new(ctx, new_target, argc, argv, dyn_knn_reg_class_id, 1);
}

JSValue dyn_knn_fit_impl(JSContext* ctx, JSValueConst this_val, int argc,
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
    dyn_fiber_offload();
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

JSValue dyn_knn_predict_impl(JSContext* ctx, JSValueConst this_val,
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
        return dyn_ml_throw_unfitted(ctx, this_val, "predict");
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
    dyn_fiber_offload();
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

#endif
