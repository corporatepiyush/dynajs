#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

JSValue dyn_scaler_new(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, int minmax)
{
    dyn_scaler_t* s = (dyn_scaler_t*)calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->minmax = minmax;
    return dyn_res_wrap(ctx, new_target, class_id, s, dyn_scaler_dispose);
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

JSValue dyn_scaler_fit_impl(JSContext* ctx, JSValueConst this_val,
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
    dyn_fiber_offload();
    if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, w)) {
        dyn_matrix_free(&mx);
        free(w);
        return JS_EXCEPTION;
    }
    dyn_matrix_free(&mx);
    free(w);
    return JS_DupValue(ctx, this_val);
}

JSValue dyn_scaler_apply(JSContext* ctx, JSValueConst this_val,
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
        dyn_fiber_offload();
        if (dyn_scaler_learn(ctx, s, mx.data, mx.rows, mx.cols, NULL)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else if (!s->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, "transform");
    } else if (mx.cols != s->n_features) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.transform: X has %u features, scaler expects %u",
            (unsigned)mx.cols, (unsigned)s->n_features);
    }
    rows = mx.rows;
    cols = mx.cols;
    flat = mx.flat;
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
    dyn_fiber_offload();
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

JSValue dyn_scaler_stat(JSContext* ctx, JSValueConst this_val,
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

JSValue dyn_ml_impute_mean(JSContext* ctx, JSValueConst this_val,
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
        dyn_fiber_offload();
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

#endif
