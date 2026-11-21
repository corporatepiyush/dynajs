#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

enum { DYN_PCA_FWD,
    DYN_PCA_FIT_FWD,
    DYN_PCA_INV };

JSValue dyn_pca_ctor(JSContext* ctx, JSValueConst new_target, int argc,
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
            double v = fabs(V[src * cols + j]);
            if (v > maxabs) {
                maxabs = v;
                maxj = j;
            }
        }
        if (V[src * cols + maxj] < 0.0)
            scale = -1.0;
        if (m->whiten) {
            double sd = sqrt(var[a]);
            scale /= (sd > 0.0) ? sd : 1.0;
        }
        for (j = 0; j < cols; j++)
            comp[a * cols + j] = V[src * cols + j] * scale;
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

JSValue dyn_pca_fit(JSContext* ctx, JSValueConst this_val, int argc,
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
    dyn_fiber_offload();
    if (dyn_pca_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

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
        dyn_fiber_offload();
        if (dyn_pca_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "transform");
    }
    in_cols = (mode == DYN_PCA_INV) ? m->n_components : m->n_features;
    out_cols = (mode == DYN_PCA_INV) ? m->n_features : m->n_components;
    if (mx.cols != in_cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.transform: X has %u columns, expected %u",
            (unsigned)mx.cols, (unsigned)in_cols);
    }
    rows = mx.rows;
    flat = mx.flat;
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
    dyn_fiber_offload();
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

JSValue dyn_pca_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FWD, 0);
}

JSValue dyn_pca_transform_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FWD, 1);
}

JSValue dyn_pca_fit_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_FIT_FWD, 0);
}

JSValue dyn_pca_inverse(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_pca_apply(ctx, t, argc, argv, DYN_PCA_INV, 0);
}

JSValue dyn_pca_components(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->comp, m->n_components, m->n_features, 0);
}

JSValue dyn_pca_mean_get(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->mean, m->n_features);
}

JSValue dyn_pca_explained(JSContext* ctx, JSValueConst this_val)
{
    dyn_pca_t* m = (dyn_pca_t*)dyn_res_native(ctx, this_val, dyn_pca_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->var, m->n_components);
}

JSValue dyn_pca_explained_ratio(JSContext* ctx, JSValueConst this_val)
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

#endif
