#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

JSValue dyn_nb_ctor(JSContext* ctx, JSValueConst new_target, int argc,
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

JSValue dyn_nb_fit(JSContext* ctx, JSValueConst this_val, int argc,
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
    dyn_fiber_offload();
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
        return dyn_ml_throw_unfitted(ctx, this_val, proba ? "predictProba" : "predict");
    }
    if ((S ? S->cols : mx.cols) != m->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->n_features);
    }
    rows = S ? S->rows : mx.rows;
    nc = m->n_classes;
    flat = !S && mx.flat;
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
    dyn_fiber_offload();
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

JSValue dyn_nb_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 0, 0);
}

JSValue dyn_nb_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 1, 0);
}

JSValue dyn_nb_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_nb_predict_impl(ctx, t, argc, argv, 0, 1);
}

JSValue dyn_nb_classes_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_nb_t* m = (dyn_nb_t*)dyn_res_native(ctx, this_val, dyn_nb_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->classes, m->n_classes);
}

#endif
