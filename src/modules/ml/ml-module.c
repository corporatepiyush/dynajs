#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#define DYN_ML_EXCL 1
#define DYN_ML_ARG_PAD 8
#define JS_ToUint32(...) (dyn_fiber_onjs(), JS_ToUint32(__VA_ARGS__))
#define JS_SetPropertyFunctionList(...) (dyn_fiber_onjs(), JS_SetPropertyFunctionList(__VA_ARGS__))
#define JS_CheckWritableBuffer(...) (dyn_fiber_onjs(), JS_CheckWritableBuffer(__VA_ARGS__))
#define JS_NewError(...) (dyn_fiber_onjs(), JS_NewError(__VA_ARGS__))
#define JS_NewString(...) (dyn_fiber_onjs(), JS_NewString(__VA_ARGS__))
#define JS_Throw(...) (dyn_fiber_onjs(), JS_Throw(__VA_ARGS__))

#define DYN_ML_ASYNC(fn, excl) \
    static JSValue fn##_async(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return dyn_ml_start(ctx, this_val, argc, argv, fn, NULL, 0, excl); }

#define DYN_CSR_GETTER(name, expr)                                       \
    static JSValue dyn_csr_##name(JSContext* ctx, JSValueConst this_val) \
    {                                                                    \
        dyn_csr_t* s = (dyn_csr_t*)dyn_res_native(ctx, this_val,         \
            dyn_csr_class_id);                                           \
        if (!s)                                                          \
            return JS_EXCEPTION;                                         \
        return (expr);                                                   \
    }

#define DYN_FOREST_FNS(prefix, class_id_var, newton_model)                     \
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,      \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_fit(ctx, t, argc, argv, class_id_var, newton_model); \
    }                                                                          \
    static JSValue prefix##_predict(JSContext* ctx, JSValueConst t, int argc,  \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_predict(ctx, t, argc, argv, class_id_var);           \
    }                                                                          \
    static JSValue prefix##_depth(JSContext* ctx, JSValueConst t)              \
    {                                                                          \
        return dyn_forest_depth(ctx, t, class_id_var);                         \
    }                                                                          \
    static JSValue prefix##_proba(JSContext* ctx, JSValueConst t, int argc,    \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_predict_proba(ctx, t, argc, argv, class_id_var);     \
    }                                                                          \
    static JSValue prefix##_apply(JSContext* ctx, JSValueConst t, int argc,    \
        JSValueConst* argv)                                                    \
    {                                                                          \
        return dyn_forest_apply(ctx, t, argc, argv, class_id_var);             \
    }                                                                          \
    static JSValue prefix##_into(JSContext* ctx, JSValueConst t,               \
        int argc, JSValueConst* argv)                                          \
    {                                                                          \
        return dyn_forest_predict_into(ctx, t, argc, argv, class_id_var);      \
    }                                                                          \
    static JSValue prefix##_imp(JSContext* ctx, JSValueConst t)                \
    {                                                                          \
        return dyn_forest_importances(ctx, t, class_id_var);                   \
    }                                                                          \
    DYN_ML_ASYNC(prefix##_fit, DYN_ML_EXCL)                                    \
    DYN_ML_ASYNC(prefix##_predict, DYN_ML_SHARED)                              \
    DYN_ML_ASYNC(prefix##_into, DYN_ML_SHARED)                                 \
    DYN_ML_ASYNC(prefix##_proba, DYN_ML_SHARED)                                \
    DYN_ML_ASYNC(prefix##_apply, DYN_ML_SHARED)

#define DYN_FOREST_PROTO(prefix)                                  \
    static const JSCFunctionListEntry prefix##_proto[] = {        \
        JS_CFUNC_DEF("fit", 2, prefix##_fit_async),               \
        JS_CFUNC_DEF("predict", 1, prefix##_predict_async),       \
        JS_CFUNC_DEF("predictInto", 2, prefix##_into_async),      \
        JS_CFUNC_DEF("predictProba", 1, prefix##_proba_async),    \
        JS_CFUNC_DEF("apply", 1, prefix##_apply_async),           \
        JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL), \
        JS_CGETSET_DEF("depth", prefix##_depth, NULL),            \
    };

#define DYN_FOREST_METHODS(prefix, class_id_var, newton_model) \
    DYN_FOREST_FNS(prefix, class_id_var, newton_model)         \
    DYN_FOREST_PROTO(prefix)

#define DYN_XGB_PROTO(prefix, class_id_var)                       \
    static JSValue prefix##_best(JSContext* ctx, JSValueConst t)  \
    {                                                             \
        return dyn_xgb_best(ctx, t, class_id_var);                \
    }                                                             \
    static const JSCFunctionListEntry prefix##_xproto[] = {       \
        JS_CFUNC_DEF("fit", 2, prefix##_fit_async),               \
        JS_CFUNC_DEF("predict", 1, prefix##_predict_async),       \
        JS_CFUNC_DEF("predictInto", 2, prefix##_into_async),      \
        JS_CFUNC_DEF("predictProba", 1, prefix##_proba_async),    \
        JS_CFUNC_DEF("apply", 1, prefix##_apply_async),           \
        JS_CGETSET_DEF("featureImportances", prefix##_imp, NULL), \
        JS_CGETSET_DEF("depth", prefix##_depth, NULL),            \
        JS_CGETSET_DEF("bestRounds", prefix##_best, NULL),        \
    };

#define DYN_KNN_METHODS(prefix, class_id_var)                                      \
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,          \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_fit_impl(ctx, t, argc, argv, class_id_var);                 \
    }                                                                              \
    static JSValue prefix##_predict(JSContext* ctx, JSValueConst t, int argc,      \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_predict_impl(ctx, t, argc, argv, class_id_var, 0);          \
    }                                                                              \
    static JSValue prefix##_predict_into(JSContext* ctx, JSValueConst t, int argc, \
        JSValueConst* argv)                                                        \
    {                                                                              \
        return dyn_knn_predict_impl(ctx, t, argc, argv, class_id_var, 1);          \
    }

#define DYN_SCALER_METHODS(prefix, class_id_var)                                         \
    static JSValue prefix##_fit(JSContext* ctx, JSValueConst t, int argc,                \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_fit_impl(ctx, t, argc, argv, class_id_var);                    \
    }                                                                                    \
    static JSValue prefix##_transform(JSContext* ctx, JSValueConst t, int argc,          \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FWD, 0);     \
    }                                                                                    \
    static JSValue prefix##_into(JSContext* ctx, JSValueConst t, int argc,               \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FWD, 1);     \
    }                                                                                    \
    static JSValue prefix##_inverse(JSContext* ctx, JSValueConst t, int argc,            \
        JSValueConst* argv)                                                              \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_INV, 0);     \
    }                                                                                    \
    static JSValue prefix##_fit_transform(JSContext* ctx, JSValueConst t,                \
        int argc, JSValueConst* argv)                                                    \
    {                                                                                    \
        return dyn_scaler_apply(ctx, t, argc, argv, class_id_var, DYN_SCALE_FIT_FWD, 0); \
    }

static double* dyn_ml_job_out(JSContext* ctx, JSValueConst target, size_t n)
{
    dyn_ml_job_t* job = dyn_ml_job();
    double* tmp;
    if (job->n_outs == job->cap_outs) {
        int cap = job->cap_outs ? job->cap_outs * 2 : 2;
        dyn_ml_out_t* no = (dyn_ml_out_t*)realloc(job->outs,
            (size_t)cap * sizeof(*no));
        if (!no) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        job->outs = no;
        job->cap_outs = cap;
    }
    tmp = (double*)malloc((n ? n : 1) * sizeof(double));
    if (!tmp) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    job->outs[job->n_outs].target = JS_DupValue(ctx, target);
    job->outs[job->n_outs].tmp = tmp;
    job->outs[job->n_outs].n = n;
    job->n_outs++;
    return tmp;
}

JSValue dyn_ml_job_entry(JSContext* ctx, void* arg)
{
    dyn_ml_job_t* job = (dyn_ml_job_t*)arg;
    JSValue r;
    int i;

    r = job->fn ? job->fn(ctx, job->this_val, job->argc, (JSValueConst*)job->argv)
                : job->fn_magic(ctx, job->this_val, job->argc,
                      (JSValueConst*)job->argv, job->magic);
    dyn_fiber_onjs();
    for (i = 0; i < job->n_outs; i++) {
        dyn_ml_out_t* o = &job->outs[i];
        if (!JS_IsException(r)) {
            double* dst;
            size_t have;
            if (dyn_ml_get_f64(ctx, o->target, &dst, &have)) {
                JS_FreeValue(ctx, r);
                r = JS_EXCEPTION;
            } else if (have < o->n) {
                JS_FreeValue(ctx, r);
                r = JS_ThrowRangeError(ctx, "dyna:ml the out array shrank while the call was in flight; nothing was written");
            } else {
                memcpy(dst, o->tmp, o->n * sizeof(double));
            }
        }
        free(o->tmp);
        JS_FreeValue(ctx, o->target);
    }
    free(job->outs);
    for (i = 0; i < job->n_holds; i++) {
        dyn_ml_hold_t* h = &job->holds[i];
        if (h->excl)
            h->r->async_users = 0;
        else if (h->r->async_users > 0)
            h->r->async_users--;
        JS_FreeValue(ctx, h->keep);
    }
    free(job->holds);
    for (i = 0; i < job->argc; i++)
        JS_FreeValue(ctx, job->argv[i]);
    free(job->argv);
    JS_FreeValue(ctx, job->this_val);
    free(job);
    return r;
}

JSValue dyn_ml_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, dyn_ml_fn* fn, dyn_ml_fn_magic* fn_magic, int magic,
    int excl_this)
{
    dyn_ml_job_t* job = (dyn_ml_job_t*)calloc(1, sizeof(*job));
    JSValue p;
    int i;
    if (!job)
        return JS_ThrowOutOfMemory(ctx);
    job->argv = (JSValue*)malloc(((size_t)argc + DYN_ML_ARG_PAD) * sizeof(JSValue));
    if (!job->argv) {
        free(job);
        return JS_ThrowOutOfMemory(ctx);
    }
    job->fn = fn;
    job->fn_magic = fn_magic;
    job->magic = magic;
    job->excl_this = excl_this;
    job->this_val = JS_DupValue(ctx, this_val);
    job->argc = argc;
    for (i = 0; i < argc; i++)
        job->argv[i] = JS_DupValue(ctx, argv[i]);
    for (i = 0; i < DYN_ML_ARG_PAD; i++)
        job->argv[argc + i] = JS_UNDEFINED;
    p = dyn_fiber_start(ctx, dyn_ml_job_entry, job);
    if (JS_IsException(p)) {
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, job->argv[i]);
        free(job->argv);
        JS_FreeValue(ctx, job->this_val);
        free(job);
    }
    return p;
}

const char* const ml_weights_keys[] = { "sampleWeight" };

static const char* const ml_sel_split_keys[] = { "seed", "shuffle", "testSize" };

static JSClassID dyn_ml_f64_class_id;

static JSClassID dyn_ml_i32_class_id;

static JSClassID dyn_ml_u32_class_id;

int dyn_ml_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    if (JS_GetClassID(v) != dyn_ml_f64_class_id) {
        JS_ThrowTypeError(ctx, "dyna:ml expected a Float64Array");
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (bpe != 8) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:ml expected a Float64Array");
        return -1;
    }
    if (JS_IsSharedArrayBuffer(buf)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:ml does not read a Float64Array over a SharedArrayBuffer; pass a private copy (new Float64Array(x))");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:ml typed array out of bounds");
        return -1;
    }
    *pp = (double*)(base + off);
    *pn = len / 8;
    return 0;
}

int dyn_ml_len(JSContext* ctx, JSValueConst v, size_t* out_len)
{
    JSValue lval;
    uint32_t len;
    int ret;

    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:ml expected an Array");
        return -1;
    }
    lval = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lval))
        return -1;
    ret = JS_ToUint32(ctx, &len, lval);
    JS_FreeValue(ctx, lval);
    if (ret)
        return -1;
    *out_len = len;
    return 0;
}

JSValueConst dyn_ml_opts(JSContext* ctx, int argc, JSValueConst* argv,
    int from)
{
    int i;
    for (i = from; i < argc; i++) {
        if (!JS_IsObject(argv[i]) || JS_IsArray(ctx, argv[i]))
            continue;
        if (JS_GetBufferKind(argv[i]) != JS_BUFFER_KIND_NONE)
            continue;
        return argv[i];
    }
    return JS_UNDEFINED;
}

int dyn_ml_reject_weights(JSContext* ctx, int argc, JSValueConst* argv,
    int from, const char* what)
{
    JSValueConst opts = dyn_ml_opts(ctx, argc, argv, from);
    JSValue wv;
    int present;

    if (JS_IsUndefined(opts))
        return 0;
    if (dyn_opts_strict(ctx, opts, ml_weights_keys, 1))
        return -1;
    wv = JS_GetPropertyStr(ctx, opts, "sampleWeight");
    if (JS_IsException(wv))
        return -1;
    present = !JS_IsUndefined(wv) && !JS_IsNull(wv);
    JS_FreeValue(ctx, wv);
    if (present) {
        JS_ThrowTypeError(ctx,
            "ml.fit: %s has no weighted fit, so sampleWeight would be ignored. "
            "Resample the rows instead, or use an estimator that supports it: "
            "LinearRegression, LogisticRegression, every tree model, "
            "XGBRegressor, XGBClassifier, KMeans, GaussianNB, StandardScaler.",
            what);
        return -1;
    }
    return 0;
}

JSClassID dyn_csr_class_id;

JSValue dyn_ml_f64array(JSContext* ctx, const double* v, size_t n)
{
    static const double zero_stub = 0.0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (n == 0)
        v = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)v, n * sizeof(double));
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(ctx, ab);
    return out;
}

JSValue dyn_ml_doubles_to_js(JSContext* ctx, const double* v, size_t n)
{
    size_t i;
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewFloat64(ctx, v[i]), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

int dyn_ml_bind_out(JSContext* ctx, JSValueConst v, size_t expect,
    double** pp)
{
    double* p;
    size_t n;

    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    if (dyn_ml_get_f64(ctx, v, &p, &n))
        return -1;
    if (n < expect) {
        JS_ThrowRangeError(ctx,
            "ml.predictInto: out has %u elements but the prediction writes %u; "
            "nothing was written",
            (unsigned)n, (unsigned)expect);
        return -1;
    }
    if (dyn_ml_job()) {
        p = dyn_ml_job_out(ctx, v, expect);
        if (!p)
            return -1;
    }
    *pp = p;
    return 0;
}

void dyn_ml_into_args(int into, int argc, JSValueConst* argv,
    JSValueConst* xa, JSValueConst* rows_arg,
    JSValueConst* cols_arg)
{
    if (into) {
        *xa = argc > 1 ? argv[1] : JS_UNDEFINED;
        *rows_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
        *cols_arg = argc > 3 ? argv[3] : JS_UNDEFINED;
    } else {
        *xa = argc > 0 ? argv[0] : JS_UNDEFINED;
        *rows_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
        *cols_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    }
}

static int dyn_ml_dbl_cmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1
                                  : 0;
}

size_t dyn_ml_classes(JSContext* ctx, const double* y, size_t rows,
    double** out)
{
    double* c = (double*)malloc((rows ? rows : 1) * sizeof(double));
    size_t n = 0, i, j;

    if (!c) {
        JS_ThrowOutOfMemory(ctx);
        return 0;
    }
    for (i = 0; i < rows; i++) {
        for (j = 0; j < n; j++)
            if (c[j] == y[i])
                break;
        if (j < n)
            continue;
        for (j = n; j > 0 && c[j - 1] > y[i]; j--)
            c[j] = c[j - 1];
        c[j] = y[i];
        n++;
        if (n == 48 && i + 1 < rows) {
            size_t r = rows - i - 1, a = 0, b = 0, m = 0;
            double* rest = (double*)malloc(r * sizeof(double));
            double* merged = rest ? (double*)malloc(rows * sizeof(double)) : NULL;
            if (!rest || !merged) {
                free(rest);
                free(merged);
                free(c);
                JS_ThrowOutOfMemory(ctx);
                return 0;
            }
            memcpy(rest, y + i + 1, r * sizeof(double));
            dyn_ml_isort(rest, r, sizeof(double), dyn_ml_dbl_cmp);
            while (a < n && b < r) {
                if (c[a] < rest[b])
                    merged[m++] = c[a++];
                else if (rest[b] < c[a])
                    merged[m++] = rest[b++];
                else {
                    merged[m++] = c[a++];
                    b++;
                }
            }
            while (a < n)
                merged[m++] = c[a++];
            while (b < r)
                merged[m++] = rest[b++];
            free(rest);
            free(c);
            *out = merged;
            return m;
        }
    }
    *out = c;
    return n;
}

static const JSClassDef dyn_csr_class = {
    "CSR",
    .finalizer = dyn_res_finalizer,
};

int dyn_ml_int_view(JSContext* ctx, JSValueConst v, const uint8_t** pp, size_t* pn, int* is_signed)
{
    JSClassID cid = JS_GetClassID(v);
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    if (cid != dyn_ml_i32_class_id && cid != dyn_ml_u32_class_id)
        return 0;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (JS_IsSharedArrayBuffer(buf)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "dyna:ml does not read an index array over a SharedArrayBuffer; pass a private copy");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (bpe != 4 || off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:ml typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len / 4;
    *is_signed = cid == dyn_ml_i32_class_id;
    return 1;
}

int dyn_ml_dense_fits(size_t rows, size_t cols)
{
    const size_t budget = (size_t)1 << 30;
    const size_t per_cell = 24;
    const size_t per_row = 280;

    if (!cols)
        cols = 1;
    if (!rows)
        rows = 1;
    if (cols > (budget - per_row) / per_cell)
        return 0;
    return rows <= budget / (per_cell * cols + per_row);
}

DYN_CSR_GETTER(rows_get, JS_NewInt64(ctx, (int64_t)s->rows))

DYN_CSR_GETTER(cols_get, JS_NewInt64(ctx, (int64_t)s->cols))

DYN_CSR_GETTER(nnz_get, JS_NewInt64(ctx, (int64_t)s->nnz))

DYN_CSR_GETTER(density_get,
    JS_NewFloat64(ctx, (s->rows && s->cols) ? (double)s->nnz / ((double)s->rows * (double)s->cols) : 0.0))

DYN_ML_ASYNC(dyn_csr_to_dense, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_csr_proto[] = {
    JS_CFUNC_DEF("toDense", 0, dyn_csr_to_dense_async),
    JS_CFUNC_DEF("row", 1, dyn_csr_row),
    JS_CGETSET_DEF("rows", dyn_csr_rows_get, NULL),
    JS_CGETSET_DEF("cols", dyn_csr_cols_get, NULL),
    JS_CGETSET_DEF("nnz", dyn_csr_nnz_get, NULL),
    JS_CGETSET_DEF("density", dyn_csr_density_get, NULL),
};

DYN_ML_ASYNC(dyn_csr_from_dense, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_csr_statics[] = {
    JS_CFUNC_DEF("fromDense", 1, dyn_csr_from_dense_async),
};

JSClassID dyn_linreg_class_id;

static const JSClassDef dyn_linreg_class = {
    "LinearRegression",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_linreg_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_linreg_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_linreg_predict_into, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_linreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_linreg_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_linreg_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_linreg_predict_into_async),
    JS_CGETSET_DEF("coef", dyn_linreg_coef_get, NULL),
    JS_CGETSET_DEF("intercept", dyn_linreg_intercept_get, NULL),
};

JSClassID dyn_logreg_class_id;

static const JSClassDef dyn_logreg_class = {
    "LogisticRegression",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_logreg_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_logreg_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_logreg_predict_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_logreg_predict_proba, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_logreg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_logreg_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_logreg_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_logreg_predict_into_async),
    JS_CFUNC_DEF("predictProba", 1, dyn_logreg_predict_proba_async),
    JS_CGETSET_DEF("classes", dyn_logreg_get_classes, NULL),
    JS_CGETSET_DEF("coef", dyn_logreg_get_coef, NULL),
    JS_CGETSET_DEF("intercept", dyn_logreg_get_intercept, NULL),
    JS_CGETSET_DEF("nIter", dyn_logreg_get_niter, NULL),
    JS_CGETSET_DEF("converged", dyn_logreg_get_converged, NULL),
};

JSClassID dyn_kmeans_class_id;

static const JSClassDef dyn_kmeans_class = {
    "KMeans",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_kmeans_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_kmeans_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_kmeans_predict_into, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_kmeans_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_kmeans_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_kmeans_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_kmeans_predict_into_async),
    JS_CGETSET_DEF("inertia", dyn_kmeans_inertia, NULL),
};

JSClassID dyn_dtc_class_id;

JSClassID dyn_dtr_class_id;

JSClassID dyn_rfc_class_id;

JSClassID dyn_rfr_class_id;

JSClassID dyn_gbr_class_id;

JSClassID dyn_gbc_class_id;

JSClassID dyn_xgbr_class_id;

JSClassID dyn_xgbc_class_id;

static const JSClassDef dyn_dtc_class = { "DecisionTreeClassifier", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_dtr_class = { "DecisionTreeRegressor", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_rfc_class = { "RandomForestClassifier", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_rfr_class = { "RandomForestRegressor", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_gbr_class = { "GradientBoostingRegressor", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_gbc_class = { "GradientBoostingClassifier", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_xgbr_class = { "XGBRegressor", .finalizer = dyn_res_finalizer };

static const JSClassDef dyn_xgbc_class = { "XGBClassifier", .finalizer = dyn_res_finalizer };

int dyn_opt_size(JSContext* ctx, JSValueConst obj, const char* name,
    size_t* out, size_t minimum)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int64_t iv;
    double dv;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToFloat64(ctx, &dv, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!isfinite(dv)) {
        JS_ThrowRangeError(ctx, "dyna:ml %s must be a finite number", name);
        return -1;
    }
    if (!(dv >= -9.2233720368547758e18 && dv < 9.2233720368547758e18)) {
        JS_ThrowRangeError(ctx, "dyna:ml %s is out of range", name);
        return -1;
    }
    iv = (int64_t)dv;
    if (iv < (int64_t)minimum) {
        JS_ThrowRangeError(ctx, "dyna:ml %s must be at least %u", name,
            (unsigned)minimum);
        return -1;
    }
#if SIZE_MAX < UINT64_MAX
    if ((uint64_t)iv > (uint64_t)SIZE_MAX) {
        JS_ThrowRangeError(ctx, "dyna:ml %s is out of range", name);
        return -1;
    }
#endif
    *out = (size_t)iv;
    return 0;
}

int dyn_opt_double(JSContext* ctx, JSValueConst obj, const char* name,
    double* out, double lo, double hi)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double dv;

    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToFloat64(ctx, &dv, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(dv >= lo && dv <= hi)) {
        JS_ThrowRangeError(ctx, "dyna:ml %s must be in [%g, %g]", name, lo, hi);
        return -1;
    }
    *out = dv;
    return 0;
}

DYN_FOREST_METHODS(dyn_dtc, dyn_dtc_class_id, 0)

DYN_FOREST_METHODS(dyn_dtr, dyn_dtr_class_id, 0)

DYN_FOREST_METHODS(dyn_rfc, dyn_rfc_class_id, 0)

DYN_FOREST_METHODS(dyn_rfr, dyn_rfr_class_id, 0)

DYN_FOREST_METHODS(dyn_gbr, dyn_gbr_class_id, 0)

DYN_FOREST_METHODS(dyn_gbc, dyn_gbc_class_id, 0)

DYN_FOREST_FNS(dyn_xgbr, dyn_xgbr_class_id, 1)

DYN_FOREST_FNS(dyn_xgbc, dyn_xgbc_class_id, 1)

DYN_XGB_PROTO(dyn_xgbr, dyn_xgbr_class_id)

DYN_XGB_PROTO(dyn_xgbc, dyn_xgbc_class_id)

static JSValue dyn_dtc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_dtc_class_id, 1, 0, 1, 0, 0);
}

static JSValue dyn_dtr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_dtr_class_id, 0, 0, 1, 0, 0);
}

static JSValue dyn_rfc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_rfc_class_id, 1, 0, 100, 0, 0);
}

static JSValue dyn_rfr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_rfr_class_id, 0, 0, 100, 0, 0);
}

static JSValue dyn_gbr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_gbr_class_id, 0, 1, 100, 3, 0);
}

static JSValue dyn_gbc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_gbc_class_id, 1, 1, 100, 3, 0);
}

static JSValue dyn_xgbr_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_xgbr_class_id, 0, 1, 100, 6, 1);
}

static JSValue dyn_xgbc_ctor(JSContext* ctx, JSValueConst nt, int argc,
    JSValueConst* argv)
{
    return dyn_forest_new(ctx, nt, argc, argv, dyn_xgbc_class_id, 1, 1, 100, 6, 1);
}

JSClassID dyn_pca_class_id;

static const JSClassDef dyn_pca_class = {
    "PCA",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_pca_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_pca_transform, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_pca_transform_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_pca_fit_transform, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_pca_inverse, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_pca_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_pca_fit_async),
    JS_CFUNC_DEF("transform", 1, dyn_pca_transform_async),
    JS_CFUNC_DEF("transformInto", 2, dyn_pca_transform_into_async),
    JS_CFUNC_DEF("fitTransform", 1, dyn_pca_fit_transform_async),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_pca_inverse_async),
    JS_CGETSET_DEF("components", dyn_pca_components, NULL),
    JS_CGETSET_DEF("mean", dyn_pca_mean_get, NULL),
    JS_CGETSET_DEF("explainedVariance", dyn_pca_explained, NULL),
    JS_CGETSET_DEF("explainedVarianceRatio", dyn_pca_explained_ratio, NULL),
};

JSClassID dyn_nb_class_id;

static const JSClassDef dyn_nb_class = {
    "GaussianNB",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_nb_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_nb_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_nb_predict_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_nb_predict_proba, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_nb_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_nb_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_nb_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_nb_predict_into_async),
    JS_CFUNC_DEF("predictProba", 1, dyn_nb_predict_proba_async),
    JS_CGETSET_DEF("classes", dyn_nb_classes_get, NULL),
};

JSClassID dyn_knn_clf_class_id;

JSClassID dyn_knn_reg_class_id;

static const JSClassDef dyn_knn_clf_class = {
    "KNClassifier",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_knn_reg_class = {
    "KNRegressor",
    .finalizer = dyn_res_finalizer,
};

DYN_KNN_METHODS(dyn_knn_clf, dyn_knn_clf_class_id)

DYN_KNN_METHODS(dyn_knn_reg, dyn_knn_reg_class_id)

DYN_ML_ASYNC(dyn_knn_clf_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_knn_clf_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_knn_clf_predict_into, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_knn_clf_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_clf_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_knn_clf_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_knn_clf_predict_into_async),
};

DYN_ML_ASYNC(dyn_knn_reg_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_knn_reg_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_knn_reg_predict_into, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_knn_reg_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_knn_reg_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_knn_reg_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_knn_reg_predict_into_async),
};

JSClassID dyn_dbscan_class_id;

static const JSClassDef dyn_dbscan_class = {
    "DBScan",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_dbscan_fit, DYN_ML_EXCL)

static const JSCFunctionListEntry dyn_dbscan_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_dbscan_fit_async),
    JS_CGETSET_DEF("labels", dyn_dbscan_labels, NULL),
    JS_CGETSET_DEF("nClusters", dyn_dbscan_nclusters, NULL),
    JS_CGETSET_DEF("eps", dyn_dbscan_eps, NULL),
};

JSClassID dyn_stdscaler_class_id;

JSClassID dyn_minmax_class_id;

static const JSClassDef dyn_stdscaler_class = {
    "StandardScaler",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_minmax_class = {
    "MinMaxScaler",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_stdscaler_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_scaler_new(ctx, new_target, dyn_stdscaler_class_id, 0);
}

static JSValue dyn_minmax_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_scaler_new(ctx, new_target, dyn_minmax_class_id, 1);
}

DYN_SCALER_METHODS(dyn_stdscaler, dyn_stdscaler_class_id)

DYN_SCALER_METHODS(dyn_minmax, dyn_minmax_class_id)

static JSValue dyn_stdscaler_mean(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 0);
}

static JSValue dyn_stdscaler_std(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_stdscaler_class_id, 1);
}

static JSValue dyn_minmax_min(JSContext* ctx, JSValueConst t)
{
    return dyn_scaler_stat(ctx, t, dyn_minmax_class_id, 0);
}

static JSValue dyn_minmax_max(JSContext* ctx, JSValueConst this_val)
{
    dyn_scaler_t* s = (dyn_scaler_t*)dyn_res_native(ctx, this_val, dyn_minmax_class_id);
    double* tmp;
    size_t j;
    JSValue out;

    if (!s)
        return JS_EXCEPTION;
    if (!s->fitted)
        return JS_NewArray(ctx);
    tmp = (double*)malloc(s->n_features * sizeof(double));
    if (!tmp)
        return JS_ThrowOutOfMemory(ctx);
    for (j = 0; j < s->n_features; j++)
        tmp[j] = s->centre[j] + s->spread[j];
    out = dyn_ml_doubles_to_js(ctx, tmp, s->n_features);
    free(tmp);
    return out;
}

DYN_ML_ASYNC(dyn_stdscaler_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_stdscaler_transform, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_stdscaler_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_stdscaler_fit_transform, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_stdscaler_inverse, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_stdscaler_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_stdscaler_fit_async),
    JS_CFUNC_DEF("transform", 1, dyn_stdscaler_transform_async),
    JS_CFUNC_DEF("transformInto", 2, dyn_stdscaler_into_async),
    JS_CFUNC_DEF("fitTransform", 1, dyn_stdscaler_fit_transform_async),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_stdscaler_inverse_async),
    JS_CGETSET_DEF("mean", dyn_stdscaler_mean, NULL),
    JS_CGETSET_DEF("std", dyn_stdscaler_std, NULL),
};

DYN_ML_ASYNC(dyn_minmax_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_minmax_transform, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_minmax_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_minmax_fit_transform, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_minmax_inverse, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_minmax_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_minmax_fit_async),
    JS_CFUNC_DEF("transform", 1, dyn_minmax_transform_async),
    JS_CFUNC_DEF("transformInto", 2, dyn_minmax_into_async),
    JS_CFUNC_DEF("fitTransform", 1, dyn_minmax_fit_transform_async),
    JS_CFUNC_DEF("inverseTransform", 1, dyn_minmax_inverse_async),
    JS_CGETSET_DEF("dataMin", dyn_minmax_min, NULL),
    JS_CGETSET_DEF("dataMax", dyn_minmax_max, NULL),
};

JSClassID dyn_svm_class_id;

static const JSClassDef dyn_svm_class = {
    "SVC",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_svm_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_svm_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_svm_predict_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_svm_decision, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_svm_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_svm_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_svm_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_svm_predict_into_async),
    JS_CFUNC_DEF("decisionFunction", 1, dyn_svm_decision_async),
    JS_CGETSET_DEF("nSupportVectors", dyn_svm_nsv, NULL),
    JS_CGETSET_DEF("classes", dyn_svm_classes_get, NULL),
};

JSClassID dyn_gmm_class_id;

static const JSClassDef dyn_gmm_class = {
    "GaussianMixture",
    .finalizer = dyn_res_finalizer,
};

DYN_ML_ASYNC(dyn_gmm_fit, DYN_ML_EXCL)

DYN_ML_ASYNC(dyn_gmm_predict, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_gmm_predict_into, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_gmm_predict_proba, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_gmm_proto[] = {
    JS_CFUNC_DEF("fit", 1, dyn_gmm_fit_async),
    JS_CFUNC_DEF("predict", 1, dyn_gmm_predict_async),
    JS_CFUNC_DEF("predictInto", 2, dyn_gmm_predict_into_async),
    JS_CFUNC_DEF("predictProba", 1, dyn_gmm_predict_proba_async),
    JS_CGETSET_DEF("weights", dyn_gmm_weights, NULL),
    JS_CGETSET_DEF("means", dyn_gmm_means, NULL),
    JS_CGETSET_DEF("variances", dyn_gmm_variances, NULL),
    JS_CGETSET_DEF("logLikelihood", dyn_gmm_loglik, NULL),
    JS_CGETSET_DEF("nIter", dyn_gmm_niter, NULL),
};

int dyn_ml_sel_opts(JSContext* ctx, JSValueConst opt, uint64_t* seed,
    int* shuffle, double* test_size, int32_t* k)
{
    JSValue v;
    if (JS_IsUndefined(opt) || JS_IsNull(opt))
        return 0;
    if (!JS_IsObject(opt)) {
        JS_ThrowTypeError(ctx, "dyna:ml options must be an object");
        return -1;
    }
#define OPT_GET(name, body)                    \
    do {                                       \
        v = JS_GetPropertyStr(ctx, opt, name); \
        if (JS_IsException(v))                 \
            return -1;                         \
        if (!JS_IsUndefined(v)) {              \
            body                               \
        }                                      \
        JS_FreeValue(ctx, v);                  \
    } while (0)

    OPT_GET("seed", { double d; int64_t sd;
                      if (JS_ToFloat64(ctx, &d, v)) { JS_FreeValue(ctx, v); return -1; }
                      if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
                          JS_ThrowRangeError(ctx, "dyna:ml seed must fit the int64 range");
                          JS_FreeValue(ctx, v);
                          return -1;
                      }
                      sd = (int64_t)d;
                      *seed = (uint64_t)sd; });
    OPT_GET("shuffle", { *shuffle = JS_ToBool(ctx, v); });
    if (test_size)
        OPT_GET("testSize", { if (JS_ToFloat64(ctx, test_size, v)) { JS_FreeValue(ctx, v); return -1; } });
    if (k) {
        OPT_GET("k", { if (JS_ToInt32(ctx, k, v)) { JS_FreeValue(ctx, v); return -1; } });
        OPT_GET("folds", { if (JS_ToInt32(ctx, k, v)) { JS_FreeValue(ctx, v); return -1; } });
    }
#undef OPT_GET
    return 0;
}

static JSValue dyn_ml_train_test_split(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t seed = 12345;
    int shuffle = 1;
    double test_size = 0.25, nd;
    uint32_t* idx = NULL;
    size_t n, ntest, i;
    JSValue r;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ml.trainTestSplit(n[, options])");
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            ml_sel_split_keys, 3))
        return JS_EXCEPTION;
    if (JS_IsObject(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToFloat64(ctx, &nd, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    } else if (JS_ToFloat64(ctx, &nd, argv[0])) {
        return JS_EXCEPTION;
    }
    if (dyn_ml_sel_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &seed, &shuffle, &test_size, NULL)
        < 0)
        return JS_EXCEPTION;
    if (!(nd >= 2.0) || nd != floor(nd) || nd > 1e8)
        return JS_ThrowRangeError(ctx, "ml.trainTestSplit: needs at least 2 samples");
    if (!(test_size > 0.0 && test_size < 1.0))
        return JS_ThrowRangeError(ctx, "ml.trainTestSplit: testSize must be in (0, 1)");
    n = (size_t)nd;
    ntest = (size_t)floor((double)n * test_size + 0.5);
    if (ntest < 1)
        ntest = 1;
    if (ntest > n - 1)
        ntest = n - 1;

    idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!idx)
        return JS_ThrowOutOfMemory(ctx);
    dyn_fiber_offload();
    for (i = 0; i < n; i++)
        idx[i] = (uint32_t)i;
    if (shuffle)
        dyn_ml_shuffle_idx(idx, n, &seed);
    r = dyn_ml_split_obj(ctx, idx + ntest, n - ntest, idx, ntest);
    free(idx);
    return r;
}

JSValue dyn_ml_vec_subset(JSContext* ctx, const double* v,
    const uint32_t* idx, size_t n)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                JS_NewFloat64(ctx, v[idx[i]]), JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static int dyn_ml_accuracy_js(JSContext* ctx, JSValueConst yTrue,
    JSValueConst yPred, double* out)
{
    size_t n, m, i, hit = 0;
    if (dyn_ml_len(ctx, yTrue, &n) || dyn_ml_len(ctx, yPred, &m))
        return -1;
    if (n != m) {
        JS_ThrowTypeError(ctx, "ml.scorer: yTrue and yPred have different lengths");
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, yTrue, (uint32_t)i);
        JSValue b = JS_GetPropertyUint32(ctx, yPred, (uint32_t)i);
        double da, db;
        int bad = JS_IsException(a) || JS_IsException(b) || JS_ToFloat64(ctx, &da, a) || JS_ToFloat64(ctx, &db, b);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (bad)
            return -1;
        if (da == db)
            hit++;
    }
    *out = n ? (double)hit / (double)n : 0.0;
    return 0;
}

int dyn_ml_apply_scorer(JSContext* ctx, JSValueConst scorer,
    JSValueConst yTrue, JSValueConst yPred,
    double* out)
{
    JSValueConst a[2];
    JSValue r;
    if (!JS_IsFunction(ctx, scorer))
        return dyn_ml_accuracy_js(ctx, yTrue, yPred, out);
    a[0] = yTrue;
    a[1] = yPred;
    r = JS_Call(ctx, scorer, JS_UNDEFINED, 2, a);
    if (JS_IsException(r))
        return -1;
    if (JS_ToFloat64(ctx, out, r)) {
        JS_FreeValue(ctx, r);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

int dyn_ml_folds(JSContext* ctx, JSValueConst opts, size_t rows,
    JSValue* out)
{
    JSValue args[2], r;
    args[0] = JS_NewInt64(ctx, (int64_t)rows);
    args[1] = JS_IsObject(opts) ? JS_DupValue(ctx, opts) : JS_UNDEFINED;
    r = dyn_ml_kfold_impl(ctx, JS_UNDEFINED, 2, (JSValueConst*)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(r))
        return -1;
    *out = r;
    return 0;
}

int dyn_ml_progress(JSContext* ctx, JSValueConst onprogress,
    size_t done, size_t total)
{
    JSValue rec, r;

    if (!JS_IsFunction(ctx, onprogress))
        return 0;
    rec = JS_NewObject(ctx);
    if (JS_IsException(rec))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, rec, "done", JS_NewInt64(ctx, (int64_t)done),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, rec, "total", JS_NewInt64(ctx, (int64_t)total),
               JS_PROP_C_W_E)
            < 0) {
        JS_FreeValue(ctx, rec);
        return -1;
    }
    r = JS_Call(ctx, onprogress, JS_UNDEFINED, 1, (JSValueConst*)&rec);
    JS_FreeValue(ctx, rec);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

DYN_ML_ASYNC_MAGIC(dyn_metric, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_metric_confusion, DYN_ML_SHARED)

DYN_ML_ASYNC_MAGIC(dyn_metric_binary, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_metric_fbeta, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_train_test_split, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_cross_val_score, DYN_ML_SHARED)

DYN_ML_ASYNC_MAGIC(dyn_ml_grid_search, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_impute_mean, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_drop_missing, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_kfold, DYN_ML_SHARED)

DYN_ML_ASYNC(dyn_ml_stratified_kfold, DYN_ML_SHARED)

DYN_ML_ASYNC_MAGIC(dyn_metric_auc, DYN_ML_SHARED)

static JSValue dyn_ml_thread_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint64_t spawned, ran_inline;
    int live;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;
    dyn_fiber_stats(&spawned, &ran_inline, &live);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "spawned", JS_NewInt64(ctx, (int64_t)spawned), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "inline", JS_NewInt64(ctx, (int64_t)ran_inline), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "live", JS_NewInt32(ctx, live), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static const JSCFunctionListEntry dyn_ml_funcs[] = {
    JS_CFUNC_DEF("threadStats", 0, dyn_ml_thread_stats),
    JS_CFUNC_MAGIC_DEF("meanSquaredError", 2, dyn_metric_async, DYN_METRIC_MSE),
    JS_CFUNC_MAGIC_DEF("meanAbsoluteError", 2, dyn_metric_async, DYN_METRIC_MAE),
    JS_CFUNC_MAGIC_DEF("r2Score", 2, dyn_metric_async, DYN_METRIC_R2),
    JS_CFUNC_MAGIC_DEF("logLoss", 2, dyn_metric_async, DYN_METRIC_LOGLOSS),
    JS_CFUNC_MAGIC_DEF("accuracy", 2, dyn_metric_async, DYN_METRIC_ACCURACY),
    JS_CFUNC_DEF("confusionMatrix", 2, dyn_metric_confusion_async),
    JS_CFUNC_MAGIC_DEF("precision", 2, dyn_metric_binary_async, DYN_BM_PRECISION),
    JS_CFUNC_MAGIC_DEF("recall", 2, dyn_metric_binary_async, DYN_BM_RECALL),
    JS_CFUNC_MAGIC_DEF("f1", 2, dyn_metric_binary_async, DYN_BM_F1),
    JS_CFUNC_MAGIC_DEF("specificity", 2, dyn_metric_binary_async, DYN_BM_SPECIFICITY),
    JS_CFUNC_MAGIC_DEF("balancedAccuracy", 2, dyn_metric_binary_async, DYN_BM_BALANCED_ACC),
    JS_CFUNC_MAGIC_DEF("matthewsCorrcoef", 2, dyn_metric_binary_async, DYN_BM_MCC),
    JS_CFUNC_MAGIC_DEF("cohenKappa", 2, dyn_metric_binary_async, DYN_BM_KAPPA),
    JS_CFUNC_DEF("fbeta", 3, dyn_metric_fbeta_async),
    JS_CFUNC_DEF("trainTestSplit", 1, dyn_ml_train_test_split_async),
    JS_CFUNC_DEF("crossValScore", 3, dyn_ml_cross_val_score_async),
    JS_CFUNC_MAGIC_DEF("gridSearch", 4, dyn_ml_grid_search_async, 0),
    JS_CFUNC_MAGIC_DEF("randomSearch", 4, dyn_ml_grid_search_async, 1),
    JS_CFUNC_DEF("imputeMean", 1, dyn_ml_impute_mean_async),
    JS_CFUNC_DEF("dropMissing", 1, dyn_ml_drop_missing_async),
    JS_CFUNC_DEF("kFold", 1, dyn_ml_kfold_async),
    JS_CFUNC_DEF("stratifiedKFold", 1, dyn_ml_stratified_kfold_async),
    JS_CFUNC_MAGIC_DEF("rocAuc", 2, dyn_metric_auc_async, 0),
    JS_CFUNC_MAGIC_DEF("averagePrecision", 2, dyn_metric_auc_async, 1),

};

JSClassID dyn_pipe_class_id;

static const JSClassDef dyn_pipe_class = {
    "Pipeline",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_pipe_mark,
};

DYN_ML_ASYNC(dyn_pipe_fit, DYN_ML_EXCL)

DYN_ML_ASYNC_MAGIC(dyn_pipe_apply, DYN_ML_SHARED)

static const JSCFunctionListEntry dyn_pipe_proto[] = {
    JS_CFUNC_DEF("fit", 2, dyn_pipe_fit_async),
    JS_CFUNC_MAGIC_DEF("predict", 1, dyn_pipe_apply_async, 0),
    JS_CFUNC_MAGIC_DEF("predictProba", 1, dyn_pipe_apply_async, 1),
    JS_CFUNC_MAGIC_DEF("transform", 1, dyn_pipe_apply_async, 2),
    JS_CFUNC_DEF("stage", 1, dyn_pipe_stage),
    JS_CGETSET_MAGIC_DEF("length", dyn_pipe_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("fitted", dyn_pipe_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("estimator", dyn_pipe_get, NULL, 2),
};

JSValue dyn_ml_throw_unfitted(JSContext* ctx, JSValueConst this_val, const char* op)
{
    static const struct {
        const JSClassID* id;
        const JSClassDef* def;
    } map[] = {
        { &dyn_linreg_class_id, &dyn_linreg_class },
        { &dyn_logreg_class_id, &dyn_logreg_class },
        { &dyn_kmeans_class_id, &dyn_kmeans_class },
        { &dyn_dtc_class_id, &dyn_dtc_class },
        { &dyn_dtr_class_id, &dyn_dtr_class },
        { &dyn_rfc_class_id, &dyn_rfc_class },
        { &dyn_rfr_class_id, &dyn_rfr_class },
        { &dyn_gbr_class_id, &dyn_gbr_class },
        { &dyn_gbc_class_id, &dyn_gbc_class },
        { &dyn_xgbr_class_id, &dyn_xgbr_class },
        { &dyn_xgbc_class_id, &dyn_xgbc_class },
        { &dyn_pca_class_id, &dyn_pca_class },
        { &dyn_nb_class_id, &dyn_nb_class },
        { &dyn_knn_clf_class_id, &dyn_knn_clf_class },
        { &dyn_knn_reg_class_id, &dyn_knn_reg_class },
        { &dyn_dbscan_class_id, &dyn_dbscan_class },
        { &dyn_stdscaler_class_id, &dyn_stdscaler_class },
        { &dyn_minmax_class_id, &dyn_minmax_class },
        { &dyn_svm_class_id, &dyn_svm_class },
        { &dyn_gmm_class_id, &dyn_gmm_class },
        { &dyn_pipe_class_id, &dyn_pipe_class },
    };
    JSClassID cid = JS_GetClassID(this_val);
    const char* name = "ml";
    char msg[160];
    JSValue err;
    size_t i;
    for (i = 0; i < countof(map); i++)
        if (*map[i].id == cid) {
            name = map[i].def->class_name;
            break;
        }
    snprintf(msg, sizeof msg, "%s.%s: call fit() first", name, op);
    err = JS_NewError(ctx);
    if (JS_IsException(err))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg),
        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return JS_Throw(ctx, err);
}

static int dyn_ml_init_module(JSContext* ctx, JSModuleDef* m)
{
    {
        JSValue ta, zero = JS_NewInt32(ctx, 0);
        JSValueConst ta_args[1];
        ta_args[0] = zero;
        ta = JS_NewTypedArray(ctx, 1, ta_args, JS_TYPED_ARRAY_FLOAT64);
        JS_FreeValue(ctx, zero);
        if (JS_IsException(ta))
            return -1;
        dyn_ml_f64_class_id = JS_GetClassID(ta);
        JS_FreeValue(ctx, ta);
        ta = JS_NewTypedArray(ctx, 1, ta_args, JS_TYPED_ARRAY_INT32);
        if (JS_IsException(ta))
            return -1;
        dyn_ml_i32_class_id = JS_GetClassID(ta);
        JS_FreeValue(ctx, ta);
        ta = JS_NewTypedArray(ctx, 1, ta_args, JS_TYPED_ARRAY_UINT32);
        if (JS_IsException(ta))
            return -1;
        dyn_ml_u32_class_id = JS_GetClassID(ta);
        JS_FreeValue(ctx, ta);
    }
    if (dyn_register_class(ctx, m, &dyn_csr_class_id, &dyn_csr_class,
            dyn_csr_proto, countof(dyn_csr_proto),
            dyn_csr_ctor, "CSR")
        < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_csr_class_id);
        JSValue ctor;
        if (JS_IsException(proto))
            return -1;
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (JS_IsException(ctor))
            return -1;
        JS_SetPropertyFunctionList(ctx, ctor, dyn_csr_statics,
            countof(dyn_csr_statics));
        JS_FreeValue(ctx, ctor);
    }
    if (dyn_register_class(ctx, m, &dyn_linreg_class_id, &dyn_linreg_class,
            dyn_linreg_proto, countof(dyn_linreg_proto),
            dyn_linreg_ctor, "LinearRegression")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_logreg_class_id, &dyn_logreg_class,
            dyn_logreg_proto, countof(dyn_logreg_proto),
            dyn_logreg_ctor, "LogisticRegression")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_kmeans_class_id, &dyn_kmeans_class,
            dyn_kmeans_proto, countof(dyn_kmeans_proto),
            dyn_kmeans_ctor, "KMeans")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_svm_class_id, &dyn_svm_class,
            dyn_svm_proto, countof(dyn_svm_proto),
            dyn_svm_ctor, "SVC")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gmm_class_id, &dyn_gmm_class,
            dyn_gmm_proto, countof(dyn_gmm_proto),
            dyn_gmm_ctor, "GaussianMixture")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtc_class_id, &dyn_dtc_class,
            dyn_dtc_proto, countof(dyn_dtc_proto),
            dyn_dtc_ctor, "DecisionTreeClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dtr_class_id, &dyn_dtr_class,
            dyn_dtr_proto, countof(dyn_dtr_proto),
            dyn_dtr_ctor, "DecisionTreeRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfc_class_id, &dyn_rfc_class,
            dyn_rfc_proto, countof(dyn_rfc_proto),
            dyn_rfc_ctor, "RandomForestClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_rfr_class_id, &dyn_rfr_class,
            dyn_rfr_proto, countof(dyn_rfr_proto),
            dyn_rfr_ctor, "RandomForestRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbr_class_id, &dyn_gbr_class,
            dyn_gbr_proto, countof(dyn_gbr_proto),
            dyn_gbr_ctor, "GradientBoostingRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_gbc_class_id, &dyn_gbc_class,
            dyn_gbc_proto, countof(dyn_gbc_proto),
            dyn_gbc_ctor, "GradientBoostingClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbr_class_id, &dyn_xgbr_class,
            dyn_xgbr_xproto, countof(dyn_xgbr_xproto),
            dyn_xgbr_ctor, "XGBRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_xgbc_class_id, &dyn_xgbc_class,
            dyn_xgbc_xproto, countof(dyn_xgbc_xproto),
            dyn_xgbc_ctor, "XGBClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pipe_class_id, &dyn_pipe_class,
            dyn_pipe_proto, countof(dyn_pipe_proto),
            dyn_pipe_ctor, "Pipeline")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_pca_class_id, &dyn_pca_class,
            dyn_pca_proto, countof(dyn_pca_proto),
            dyn_pca_ctor, "PCA")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_nb_class_id, &dyn_nb_class,
            dyn_nb_proto, countof(dyn_nb_proto),
            dyn_nb_ctor, "GaussianNB")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_clf_class_id, &dyn_knn_clf_class,
            dyn_knn_clf_proto, countof(dyn_knn_clf_proto),
            dyn_knn_clf_ctor, "KNClassifier")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_knn_reg_class_id, &dyn_knn_reg_class,
            dyn_knn_reg_proto, countof(dyn_knn_reg_proto),
            dyn_knn_reg_ctor, "KNRegressor")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_dbscan_class_id, &dyn_dbscan_class,
            dyn_dbscan_proto, countof(dyn_dbscan_proto),
            dyn_dbscan_ctor, "DBScan")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_stdscaler_class_id,
            &dyn_stdscaler_class, dyn_stdscaler_proto,
            countof(dyn_stdscaler_proto),
            dyn_stdscaler_ctor, "StandardScaler")
        < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_minmax_class_id, &dyn_minmax_class,
            dyn_minmax_proto, countof(dyn_minmax_proto),
            dyn_minmax_ctor, "MinMaxScaler")
        < 0)
        return -1;
    if (dyn_ml_install_persistence(ctx) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_ml_funcs, countof(dyn_ml_funcs));
}

int js_nat_init_ml(JSContext* ctx)
{
    JSModuleDef* m;
    m = JS_NewCModule(ctx, "dyna:ml", dyn_ml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "CSR");
    JS_AddModuleExport(ctx, m, "LinearRegression");
    JS_AddModuleExport(ctx, m, "LogisticRegression");
    JS_AddModuleExport(ctx, m, "KMeans");
    JS_AddModuleExport(ctx, m, "SVC");
    JS_AddModuleExport(ctx, m, "GaussianMixture");
    JS_AddModuleExport(ctx, m, "DecisionTreeClassifier");
    JS_AddModuleExport(ctx, m, "DecisionTreeRegressor");
    JS_AddModuleExport(ctx, m, "RandomForestClassifier");
    JS_AddModuleExport(ctx, m, "RandomForestRegressor");
    JS_AddModuleExport(ctx, m, "GradientBoostingRegressor");
    JS_AddModuleExport(ctx, m, "GradientBoostingClassifier");
    JS_AddModuleExport(ctx, m, "XGBRegressor");
    JS_AddModuleExport(ctx, m, "XGBClassifier");
    JS_AddModuleExport(ctx, m, "PCA");
    JS_AddModuleExport(ctx, m, "GaussianNB");
    JS_AddModuleExport(ctx, m, "KNClassifier");
    JS_AddModuleExport(ctx, m, "KNRegressor");
    JS_AddModuleExport(ctx, m, "DBScan");
    JS_AddModuleExport(ctx, m, "Pipeline");
    JS_AddModuleExport(ctx, m, "StandardScaler");
    JS_AddModuleExport(ctx, m, "MinMaxScaler");
    return JS_AddModuleExportList(ctx, m, dyn_ml_funcs, countof(dyn_ml_funcs));
}

#endif
