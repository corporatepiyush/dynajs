#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

JSValue dyn_kmeans_ctor(JSContext* ctx, JSValueConst new_target,
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

static int dyn_ml_cl_poll(void* ud)
{
    return JS_CheckInterrupt((JSContext*)ud);
}

static int dyn_kmeans_train(JSContext* ctx, dyn_kmeans_t* m, const double* X,
    size_t rows, size_t cols, const double* w)
{
    switch (mlk_kmeans_train(m, X, rows, cols, w, dyn_ml_cl_poll, ctx)) {
    case MLK_CL_OK:
        return 0;
    case MLK_CL_E_FEW_ROWS:
        JS_ThrowRangeError(ctx, "ml.fit: KMeans needs at least nClusters rows");
        return -1;
    case MLK_CL_E_K_CLOSE:
        JS_ThrowRangeError(ctx, "ml.fit: KMeans nClusters too close to the number of rows");
        return -1;
    case MLK_CL_E_NOMEM:
        JS_ThrowOutOfMemory(ctx);
        return -1;
    default:
        return -1;
    }
}

JSValue dyn_kmeans_fit(JSContext* ctx, JSValueConst this_val,
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
    dyn_fiber_offload();
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
    dyn_fiber_offload();
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

JSValue dyn_kmeans_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_kmeans_predict_impl(ctx, this_val, argc, argv, 0);
}

JSValue dyn_kmeans_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return dyn_kmeans_predict_impl(ctx, this_val, argc, argv, 1);
}

JSValue dyn_kmeans_inertia(JSContext* ctx, JSValueConst this_val)
{
    dyn_kmeans_t* m = (dyn_kmeans_t*)dyn_res_native(ctx, this_val, dyn_kmeans_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->inertia : 0.0);
}

#endif
