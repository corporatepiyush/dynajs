#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

static const char* const ml_gmm_keys[] = { "seed", "maxIter", "tol",
    "regCovar" };

JSValue dyn_gmm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_gmm_t* m;
    int64_t k = 3;
    size_t max_iter = 200, seed_holder = 12345;
    double tol = 1e-3, reg = 1e-6;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &k, argv[0]))
            return JS_EXCEPTION;
        if (k < 1)
            return JS_ThrowRangeError(ctx, "ml.GaussianMixture: nComponents must be at least 1");
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], ml_gmm_keys, 4))
            return JS_EXCEPTION;
        if (dyn_opt_size(ctx, argv[1], "maxIter", &max_iter, 1) || dyn_opt_size(ctx, argv[1], "seed", &seed_holder, 0) || dyn_opt_double(ctx, argv[1], "tol", &tol, 0.0, 1e12) || dyn_opt_double(ctx, argv[1], "regCovar", &reg, 0.0, 1e12))
            return JS_EXCEPTION;
    } else if (argc > 1 && !JS_IsUndefined(argv[1])) {
        return JS_ThrowTypeError(ctx, "ml.GaussianMixture: expected an options object");
    }
    if (max_iter > DYN_ML_MAX_EM_ITERS)
        return JS_ThrowRangeError(ctx, "ml.GaussianMixture: maxIter must be at most %u",
            (unsigned)DYN_ML_MAX_EM_ITERS);
    m = (dyn_gmm_t*)calloc(1, sizeof(*m));
    if (!m)
        return JS_ThrowOutOfMemory(ctx);
    m->k = (size_t)k;
    m->max_iter = max_iter;
    m->tol = tol;
    m->reg = reg;
    m->seed = (uint64_t)seed_holder;
    return dyn_res_wrap(ctx, new_target, dyn_gmm_class_id, m, dyn_gmm_dispose);
}

static int dyn_gmm_poll(void* ud)
{
    return JS_CheckInterrupt((JSContext*)ud);
}

static int dyn_gmm_learn(JSContext* ctx, dyn_gmm_t* m, const double* X,
    size_t rows, size_t cols)
{
    switch (mlk_gmm_learn(m, X, rows, cols, dyn_gmm_poll, ctx)) {
    case MLK_CL_OK:
        return 0;
    case MLK_CL_E_FEW_ROWS:
        JS_ThrowRangeError(ctx,
            "ml.fit: GaussianMixture needs at least nComponents rows");
        return -1;
    case MLK_CL_E_TOO_BIG:
        JS_ThrowRangeError(ctx, "ml.fit: data too large for GaussianMixture");
        return -1;
    case MLK_CL_E_NOMEM:
        JS_ThrowOutOfMemory(ctx);
        return -1;
    default:
        return -1;
    }
}

JSValue dyn_gmm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    if (dyn_ml_reject_weights(ctx, argc, argv, 1, "GaussianMixture"))
        return JS_EXCEPTION;
    dyn_gmm_t* m;
    dyn_matrix_t mx = { 0 };
    JSValueConst rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (dyn_ml_ingest_X(ctx, argv[0], rows_arg, cols_arg, &mx))
        return JS_EXCEPTION;
    m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (dyn_ml_check_finite(ctx, &mx, NULL)) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    {
        double *ow = m->weight, *om = m->mean, *ov = m->var;
        double *oi = m->inv_var, *ol = m->lognorm;
        size_t ocols = m->cols, oiter = m->n_iter;
        double oll = m->loglik;
        dyn_fiber_offload();
        if (dyn_gmm_learn(ctx, m, mx.data, mx.rows, mx.cols)) {
            m->weight = ow;
            m->mean = om;
            m->var = ov;
            m->inv_var = oi;
            m->lognorm = ol;
            m->cols = ocols;
            m->n_iter = oiter;
            m->loglik = oll;
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
        free(ow);
        free(om);
        free(ov);
        free(oi);
        free(ol);
    }
    dyn_matrix_free(&mx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_gmm_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int proba,
    int into)
{
    dyn_gmm_t* m;
    dyn_matrix_t mx = { 0 };
    dyn_csr_t* S = NULL;
    dyn_ml_row_src rs = { 0 };
    double* out = NULL;
    size_t rows, i, c;
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
    m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m) {
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (!m->fitted) {
        dyn_matrix_free(&mx);
        return dyn_ml_throw_unfitted(ctx, this_val, proba ? "predictProba" : "predict");
    }
    if ((S ? S->cols : mx.cols) != m->cols) {
        dyn_matrix_free(&mx);
        return JS_ThrowTypeError(ctx, "ml.predict: X has %llu features, model expects %llu",
            (unsigned long long)(S ? S->cols : mx.cols),
            (unsigned long long)m->cols);
    }
    rows = S ? S->rows : mx.rows;
    flat = !S && mx.flat;
    if (proba && (m->k == 0 || rows > (SIZE_MAX / sizeof(double)) / m->k)) {
        dyn_matrix_free(&mx);
        return JS_ThrowRangeError(ctx, "ml.predict: data too large for predictProba");
    }
    if (into) {
        if (proba) {
            dyn_matrix_free(&mx);
            return JS_ThrowTypeError(ctx,
                "ml.predictInto: label prediction only; predictProba's rows x "
                "components matrix keeps its Array form");
        }
        if (dyn_ml_bind_out(ctx, argv[0], rows, &out)) {
            dyn_matrix_free(&mx);
            return JS_EXCEPTION;
        }
    } else {
        out = (double*)malloc(rows * (proba ? m->k : 1) * sizeof(double));
        if (!out) {
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    dyn_fiber_offload();
    if (dyn_ml_row_src_init(ctx, &rs, &mx, S)) {
        if (!into)
            free(out);
        dyn_matrix_free(&mx);
        return JS_EXCEPTION;
    }
    if (proba) {
        for (i = 0; i < rows; i++)
            dyn_gmm_estep(m, dyn_ml_row_at(&rs, i), 1, out + i * m->k);
    } else {
        double* sc = (double*)malloc(m->k * sizeof(double));
        if (!sc) {
            if (!into)
                free(out);
            dyn_ml_row_src_free(&rs);
            dyn_matrix_free(&mx);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < rows; i++) {
            const double* xi = dyn_ml_row_at(&rs, i);
            double best = -HUGE_VAL;
            size_t bestc = 0;
            for (c = 0; c < m->k; c++) {
                sc[c] = dyn_gmm_logpdf(m, xi, c);
                if (sc[c] > best) {
                    best = sc[c];
                    bestc = c;
                }
            }
            out[i] = (double)bestc;
        }
        free(sc);
    }
    dyn_ml_row_src_free(&rs);
    dyn_matrix_free(&mx);
    if (into)
        return JS_NewInt64(ctx, (int64_t)rows);
    result = proba ? dyn_ml_matrix_to_js(ctx, out, rows, m->k, flat)
                   : dyn_ml_predict_result(ctx, out, rows, as_f64);
    free(out);
    return result;
}

JSValue dyn_gmm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 0, 0);
}

JSValue dyn_gmm_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 1, 0);
}

JSValue dyn_gmm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv)
{
    return dyn_gmm_predict_impl(ctx, t, argc, argv, 0, 1);
}

JSValue dyn_gmm_weights(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_doubles_to_js(ctx, m->weight, m->k);
}

JSValue dyn_gmm_means(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->mean, m->k, m->cols, 0);
}

JSValue dyn_gmm_variances(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    if (!m->fitted)
        return JS_NewArray(ctx);
    return dyn_ml_matrix_to_js(ctx, m->var, m->k, m->cols, 0);
}

JSValue dyn_gmm_loglik(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, m->fitted ? m->loglik : 0.0);
}

JSValue dyn_gmm_niter(JSContext* ctx, JSValueConst this_val)
{
    dyn_gmm_t* m = (dyn_gmm_t*)dyn_res_native(ctx, this_val, dyn_gmm_class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->n_iter : 0));
}

#endif
