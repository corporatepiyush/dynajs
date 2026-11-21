#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

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

JSValue dyn_dbscan_ctor(JSContext* ctx, JSValueConst new_target,
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

static int dyn_dbscan_run(JSContext* ctx, dyn_dbscan_t* m, const double* X,
    size_t rows, size_t cols, int* labels)
{
    int r = mlk_dbscan_run(m, X, rows, cols, labels);
    if (r < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return r;
}

JSValue dyn_dbscan_fit(JSContext* ctx, JSValueConst this_val, int argc,
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
    dyn_fiber_offload();
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

JSValue dyn_dbscan_labels(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_ints_to_js(ctx, m->labels, m->rows);
}

JSValue dyn_dbscan_nclusters(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, m->fitted ? m->n_clusters : 0);
}

JSValue dyn_dbscan_eps(JSContext* ctx, JSValueConst this_val)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)dyn_res_native(ctx, this_val, dyn_dbscan_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->eps);
}

#endif
