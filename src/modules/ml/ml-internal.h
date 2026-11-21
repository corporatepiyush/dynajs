#ifndef ML_INTERNAL_H
#define ML_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"
#include "core/dyn-prng.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

JSValue dyn_ml_throw_unfitted(JSContext* ctx, JSValueConst this_val, const char* op);

typedef JSValue dyn_ml_fn(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

typedef JSValue dyn_ml_fn_magic(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

typedef struct {
    DynResource* r;
    int excl;
    JSValue keep;
} dyn_ml_hold_t;

typedef struct {
    JSValue target;
    double* tmp;
    size_t n;
} dyn_ml_out_t;

typedef struct {
    dyn_ml_fn* fn;
    dyn_ml_fn_magic* fn_magic;
    int magic;
    int excl_this;
    JSValue this_val;
    int argc;
    JSValue* argv;
    dyn_ml_hold_t* holds;
    int n_holds, cap_holds;
    dyn_ml_out_t* outs;
    int n_outs, cap_outs;
} dyn_ml_job_t;

#define DYN_ML_SHARED 0

JSValue dyn_ml_job_entry(JSContext* ctx, void* arg);

static inline dyn_ml_job_t* dyn_ml_job(void)
{
    return (dyn_ml_job_t*)dyn_fiber_arg(dyn_ml_job_entry);
}

DynResource* dyn_ml_res_get(JSContext* ctx, JSValueConst v,
    JSClassID class_id);

static inline void* dyn_ml_res_native(JSContext* ctx, JSValueConst v, JSClassID class_id)
{
    DynResource* r = dyn_ml_res_get(ctx, v, class_id);
    return r ? r->native : NULL;
}

int dyn_ml_interrupt(JSContext* ctx);

#ifndef DYN_ML_RAW_ENGINE
#define dyn_res_get(c, v, id) dyn_ml_res_get(c, v, id)
#endif

#define dyn_res_native(c, v, id) dyn_ml_res_native(c, v, id)

#ifndef DYN_ML_RAW_ENGINE
#define JS_CheckInterrupt(c) dyn_ml_interrupt(c)
#endif

#define JS_FreeValue(...) (dyn_fiber_onjs(), JS_FreeValue(__VA_ARGS__))

#ifndef DYN_ML_RAW_ENGINE
#define JS_ThrowTypeError(...) (dyn_fiber_onjs(), JS_ThrowTypeError(__VA_ARGS__))
#endif

#define JS_ThrowRangeError(...) (dyn_fiber_onjs(), JS_ThrowRangeError(__VA_ARGS__))

#ifndef DYN_ML_RAW_ENGINE
#define JS_ThrowOutOfMemory(...) (dyn_fiber_onjs(), JS_ThrowOutOfMemory(__VA_ARGS__))
#endif

#define JS_GetPropertyStr(...) (dyn_fiber_onjs(), JS_GetPropertyStr(__VA_ARGS__))
#define JS_NewArray(...) (dyn_fiber_onjs(), JS_NewArray(__VA_ARGS__))
#define JS_NewInt64(...) (dyn_fiber_onjs(), JS_NewInt64(__VA_ARGS__))

#ifndef DYN_ML_RAW_ENGINE
#define JS_DupValue(...) (dyn_fiber_onjs(), JS_DupValue(__VA_ARGS__))
#endif

#define JS_ToFloat64(...) (dyn_fiber_onjs(), JS_ToFloat64(__VA_ARGS__))

#ifndef DYN_ML_RAW_ENGINE
#define JS_ThrowInternalError(...) (dyn_fiber_onjs(), JS_ThrowInternalError(__VA_ARGS__))
#endif

#define JS_DefinePropertyValueStr(...) (dyn_fiber_onjs(), JS_DefinePropertyValueStr(__VA_ARGS__))
#define JS_ToInt64(...) (dyn_fiber_onjs(), JS_ToInt64(__VA_ARGS__))
#define JS_NewFloat64(...) (dyn_fiber_onjs(), JS_NewFloat64(__VA_ARGS__))
#define JS_IsArray(...) (dyn_fiber_onjs(), JS_IsArray(__VA_ARGS__))
#define JS_FreeCString(...) (dyn_fiber_onjs(), JS_FreeCString(__VA_ARGS__))
#define dyn_res_wrap(...) (dyn_fiber_onjs(), dyn_res_wrap(__VA_ARGS__))
#define JS_IsFunction(...) (dyn_fiber_onjs(), JS_IsFunction(__VA_ARGS__))
#define JS_GetPropertyUint32(...) (dyn_fiber_onjs(), JS_GetPropertyUint32(__VA_ARGS__))
#define JS_DefinePropertyValueUint32(...) (dyn_fiber_onjs(), JS_DefinePropertyValueUint32(__VA_ARGS__))
#define JS_Call(c, ...) dyn_fiber_await((c), (dyn_fiber_onjs(), JS_Call((c), __VA_ARGS__)))
#define JS_ToCString(...) (dyn_fiber_onjs(), JS_ToCString(__VA_ARGS__))
#define JS_NewObject(...) (dyn_fiber_onjs(), JS_NewObject(__VA_ARGS__))
#define JS_NewInt32(...) (dyn_fiber_onjs(), JS_NewInt32(__VA_ARGS__))
#define JS_ToInt32(...) (dyn_fiber_onjs(), JS_ToInt32(__VA_ARGS__))
#define JS_NewTypedArray(...) (dyn_fiber_onjs(), JS_NewTypedArray(__VA_ARGS__))
#define JS_GetException(...) (dyn_fiber_onjs(), JS_GetException(__VA_ARGS__))
#define JS_GetArrayBuffer(...) (dyn_fiber_onjs(), JS_GetArrayBuffer(__VA_ARGS__))
#define JS_ToBool(...) (dyn_fiber_onjs(), JS_ToBool(__VA_ARGS__))
#define JS_NewUint32(...) (dyn_fiber_onjs(), JS_NewUint32(__VA_ARGS__))
#define JS_NewBool(...) (dyn_fiber_onjs(), JS_NewBool(__VA_ARGS__))
#define JS_NewArrayBufferCopy(...) (dyn_fiber_onjs(), JS_NewArrayBufferCopy(__VA_ARGS__))
#define JS_GetTypedArrayBuffer(...) (dyn_fiber_onjs(), JS_GetTypedArrayBuffer(__VA_ARGS__))
#define dyn_opts_strict(...) (dyn_fiber_onjs(), dyn_opts_strict(__VA_ARGS__))
#define JS_GetClassProto(...) (dyn_fiber_onjs(), JS_GetClassProto(__VA_ARGS__))
#define js_malloc(...) (dyn_fiber_onjs(), js_malloc(__VA_ARGS__))
#define JS_AtomToCString(...) (dyn_fiber_onjs(), JS_AtomToCString(__VA_ARGS__))

int dyn_ml_get_f64(JSContext* ctx, JSValueConst v, double** pp, size_t* pn);

JSValue dyn_ml_start(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, dyn_ml_fn* fn, dyn_ml_fn_magic* fn_magic, int magic,
    int excl_this);

#define DYN_ML_ASYNC_MAGIC(fn, excl) \
    static JSValue fn##_async(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) { return dyn_ml_start(ctx, this_val, argc, argv, NULL, fn, magic, excl); }

extern const char* const ml_weights_keys[1];

#include "core/dyn-mlk.h"

typedef struct {
    double* data;
    size_t rows, cols;
    int owned;
    int flat;
} dyn_matrix_t;

void dyn_matrix_free(dyn_matrix_t* mx);

int dyn_ml_len(JSContext* ctx, JSValueConst v, size_t* out_len);

int dyn_ml_ingest_matrix_array(JSContext* ctx, JSValueConst x,
    dyn_matrix_t* mx);

int dyn_ml_ingest_vector(JSContext* ctx, JSValueConst y, size_t expect,
    double** pout);

static inline int dyn_ml_any_len(JSContext* ctx, JSValueConst v, size_t* pn)
{
    double* tmp;
    if (JS_IsArray(ctx, v))
        return dyn_ml_len(ctx, v, pn);
    return dyn_ml_get_f64(ctx, v, &tmp, pn);
}

int dyn_ml_ingest_any_vector(JSContext* ctx, JSValueConst v,
    double** pout, size_t* pn);

int dyn_ml_check_finite(JSContext* ctx, const dyn_matrix_t* mx,
    const double* y);

int dyn_ml_check_finite_fast(JSContext* ctx, const dyn_matrix_t* mx);

JSValueConst dyn_ml_opts(JSContext* ctx, int argc, JSValueConst* argv,
    int from);

int dyn_ml_ingest_weights(JSContext* ctx, int argc, JSValueConst* argv,
    int from, size_t rows, double** pw);

int dyn_ml_reject_weights(JSContext* ctx, int argc, JSValueConst* argv,
    int from, const char* what);

extern JSClassID dyn_csr_class_id;

int dyn_ml_ingest_Xy_ex(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** py, double** pw,
    int missing_ok);

int dyn_ml_ingest_Xy(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** py, double** pw);

int dyn_ml_ingest_Xy_no_w(JSContext* ctx, JSValueConst xv,
    JSValueConst yv, JSValueConst rows_arg,
    JSValueConst cols_arg, dyn_matrix_t* mx,
    double** py);

int dyn_ml_ingest_X(JSContext* ctx, JSValueConst xv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    dyn_matrix_t* mx);

int dyn_ml_ingest_X_w(JSContext* ctx, JSValueConst xv,
    JSValueConst rows_arg, JSValueConst cols_arg,
    int argc, JSValueConst* argv, int opt_from,
    dyn_matrix_t* mx, double** pw);

JSValue dyn_ml_f64array(JSContext* ctx, const double* v, size_t n);

JSValue dyn_ml_matrix_to_js(JSContext* ctx, const double* v, size_t rows,
    size_t cols, int flat);

JSValue dyn_ml_doubles_to_js(JSContext* ctx, const double* v, size_t n);

int dyn_ml_parse_as_opt(JSContext* ctx, int argc, JSValueConst* argv,
    int from, int* pas_f64);

int dyn_ml_bind_out(JSContext* ctx, JSValueConst v, size_t expect,
    double** pp);

static inline JSValue dyn_ml_predict_result(JSContext* ctx, const double* v, size_t n,
    int as_f64)
{
    return as_f64 ? dyn_ml_f64array(ctx, v, n) : dyn_ml_doubles_to_js(ctx, v, n);
}

void dyn_ml_into_args(int into, int argc, JSValueConst* argv,
    JSValueConst* xa, JSValueConst* rows_arg,
    JSValueConst* cols_arg);

size_t dyn_ml_classes(JSContext* ctx, const double* y, size_t rows,
    double** out);

int dyn_ml_int_view(JSContext* ctx, JSValueConst v, const uint8_t** pp, size_t* pn, int* is_signed);

static inline double dyn_ml_int_view_at(const uint8_t* p, size_t i, int is_signed)
{
    uint32_t u;
    memcpy(&u, p + i * 4, 4);
    return is_signed ? (double)(int32_t)u : (double)u;
}

int dyn_ml_ingest_idx_vector(JSContext* ctx, JSValueConst v, double** pout, size_t* pn);

JSValue dyn_csr_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_csr_from_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dyn_ml_dense_fits(size_t rows, size_t cols);

JSValue dyn_csr_to_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_csr_row(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline dyn_csr_t* dyn_ml_as_csr(JSContext* ctx, JSValueConst v)
{
    if (JS_GetClassID(v) != dyn_csr_class_id)
        return NULL;
    return (dyn_csr_t*)dyn_res_native(ctx, v, dyn_csr_class_id);
}

int dyn_ml_check_csr_finite(JSContext* ctx, const dyn_csr_t* S,
    const double* y);

typedef struct {
    const double* dense;
    const dyn_csr_t* S;
    double* scratch;
    size_t rows, cols;
} dyn_ml_row_src;

int dyn_ml_row_src_init(JSContext* ctx, dyn_ml_row_src* rs,
    const dyn_matrix_t* mx, const dyn_csr_t* S);

const double* dyn_ml_row_at(const dyn_ml_row_src* rs, size_t i);

void dyn_ml_row_src_free(dyn_ml_row_src* rs);

int dyn_ml_check_csr_values_finite(JSContext* ctx, const dyn_csr_t* S);

extern JSClassID dyn_linreg_class_id;

JSValue dyn_linreg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_linreg_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_linreg_coef_get(JSContext* ctx, JSValueConst this_val);

JSValue dyn_linreg_intercept_get(JSContext* ctx, JSValueConst this_val);

JSValue dyn_linreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_linreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_logreg_class_id;

int dyn_opt_size(JSContext* ctx, JSValueConst obj, const char* name,
    size_t* out, size_t minimum);

int dyn_opt_double(JSContext* ctx, JSValueConst obj, const char* name,
    double* out, double lo, double hi);

JSValue dyn_logreg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_logreg_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logreg_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logreg_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logreg_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logreg_get_classes(JSContext* ctx, JSValueConst t);

JSValue dyn_logreg_get_niter(JSContext* ctx, JSValueConst t);

JSValue dyn_logreg_get_converged(JSContext* ctx, JSValueConst t);

JSValue dyn_logreg_get_coef(JSContext* ctx, JSValueConst t);

JSValue dyn_logreg_get_intercept(JSContext* ctx, JSValueConst t);

extern JSClassID dyn_kmeans_class_id;

JSValue dyn_kmeans_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_kmeans_fit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_kmeans_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_kmeans_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_kmeans_inertia(JSContext* ctx, JSValueConst this_val);

int dyn_tree_fit_one(JSContext* ctx, dyn_tree_t* t, const double* X,
    const double* y, size_t rows, size_t cols,
    const double* classes, size_t n_classes,
    const dyn_tree_opts_t* opt, uint64_t* rng,
    uint32_t* idx, size_t n, const dyn_tree_work_t* w);

extern JSClassID dyn_dtc_class_id;

extern JSClassID dyn_dtr_class_id;

extern JSClassID dyn_rfc_class_id;

extern JSClassID dyn_rfr_class_id;

extern JSClassID dyn_gbr_class_id;

extern JSClassID dyn_gbc_class_id;

extern JSClassID dyn_xgbr_class_id;

extern JSClassID dyn_xgbc_class_id;

JSValue dyn_forest_new(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv,
    JSClassID class_id, int classifier, int boosting,
    size_t default_trees, size_t default_depth,
    int newton);

JSValue dyn_forest_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, JSClassID class_id,
    int newton);

JSValue dyn_forest_predict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id);

JSValue dyn_forest_predict_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id);

JSValue dyn_forest_predict_proba(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id);

JSValue dyn_forest_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id);

JSValue dyn_forest_importances(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id);

JSValue dyn_forest_depth(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id);

JSValue dyn_xgb_best(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id);

extern JSClassID dyn_pca_class_id;

JSValue dyn_pca_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_pca_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_pca_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_pca_transform_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_pca_fit_transform(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_pca_inverse(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_pca_components(JSContext* ctx, JSValueConst this_val);

JSValue dyn_pca_mean_get(JSContext* ctx, JSValueConst this_val);

JSValue dyn_pca_explained(JSContext* ctx, JSValueConst this_val);

JSValue dyn_pca_explained_ratio(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_nb_class_id;

JSValue dyn_nb_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_nb_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_nb_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_nb_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_nb_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_nb_classes_get(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_knn_clf_class_id;

extern JSClassID dyn_knn_reg_class_id;

JSValue dyn_knn_clf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_knn_reg_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_knn_fit_impl(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, JSClassID class_id);

JSValue dyn_knn_predict_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int into);

_Static_assert(sizeof(dyn_dbscan_t) <= 48,
    "dyn_dbscan_t regained padding: reorder largest-first");

extern JSClassID dyn_dbscan_class_id;

JSValue dyn_dbscan_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_dbscan_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_dbscan_labels(JSContext* ctx, JSValueConst this_val);

JSValue dyn_dbscan_nclusters(JSContext* ctx, JSValueConst this_val);

JSValue dyn_dbscan_eps(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_stdscaler_class_id;

extern JSClassID dyn_minmax_class_id;

JSValue dyn_scaler_new(JSContext* ctx, JSValueConst new_target,
    JSClassID class_id, int minmax);

JSValue dyn_scaler_fit_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id);

enum { DYN_SCALE_FWD,
    DYN_SCALE_INV,
    DYN_SCALE_FIT_FWD };

JSValue dyn_scaler_apply(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    JSClassID class_id, int mode, int into);

JSValue dyn_scaler_stat(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id, int which);

extern JSClassID dyn_svm_class_id;

JSValue dyn_svm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_svm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_svm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_svm_decision(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_svm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_svm_nsv(JSContext* ctx, JSValueConst this_val);

JSValue dyn_svm_classes_get(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_gmm_class_id;

JSValue dyn_gmm_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_gmm_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_gmm_predict(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_gmm_predict_proba(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_gmm_predict_into(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_gmm_weights(JSContext* ctx, JSValueConst this_val);

JSValue dyn_gmm_means(JSContext* ctx, JSValueConst this_val);

JSValue dyn_gmm_variances(JSContext* ctx, JSValueConst this_val);

JSValue dyn_gmm_loglik(JSContext* ctx, JSValueConst this_val);

JSValue dyn_gmm_niter(JSContext* ctx, JSValueConst this_val);

enum { DYN_METRIC_MSE,
    DYN_METRIC_MAE,
    DYN_METRIC_R2,
    DYN_METRIC_LOGLOSS,
    DYN_METRIC_ACCURACY };

int dyn_ml_is_matrix(JSContext* ctx, JSValueConst v);

JSValue dyn_metric(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

enum { DYN_BM_PRECISION,
    DYN_BM_RECALL,
    DYN_BM_F1,
    DYN_BM_SPECIFICITY,
    DYN_BM_BALANCED_ACC,
    DYN_BM_MCC,
    DYN_BM_KAPPA };

JSValue dyn_metric_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_metric_fbeta(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_metric_auc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_pr);

static inline void dyn_ml_shuffle_idx(uint32_t* idx, size_t n, uint64_t* state)
{
    size_t i;
    for (i = n; i > 1; i--) {
        size_t j = (size_t)(dyn_splitmix64(state) % (uint64_t)i);
        uint32_t t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
}

JSValue dyn_ml_split_obj(JSContext* ctx, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte);

int dyn_ml_sel_opts(JSContext* ctx, JSValueConst opt, uint64_t* seed,
    int* shuffle, double* test_size, int32_t* k);

JSValue dyn_ml_kfold_impl(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ml_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ml_stratified_kfold(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_metric_confusion(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ml_rows_subset(JSContext* ctx, const dyn_matrix_t* mx,
    const uint32_t* idx, size_t n);

JSValue dyn_ml_vec_subset(JSContext* ctx, const double* v,
    const uint32_t* idx, size_t n);

int dyn_ml_apply_scorer(JSContext* ctx, JSValueConst scorer,
    JSValueConst yTrue, JSValueConst yPred,
    double* out);

int dyn_ml_score_fold(JSContext* ctx, JSValueConst factory,
    JSValueConst scorer, const dyn_matrix_t* mx,
    const double* y, const uint32_t* tr, size_t ntr,
    const uint32_t* te, size_t nte, double* out);

int dyn_ml_folds(JSContext* ctx, JSValueConst opts, size_t rows,
    JSValue* out);

int dyn_ml_read_idx(JSContext* ctx, JSValueConst arr, uint32_t** out,
    size_t* n, size_t rows);

int dyn_ml_search_ingest(JSContext* ctx, JSValueConst xv, JSValueConst yv,
    dyn_matrix_t* mx, double** py);

int dyn_ml_parse_jobs(JSContext* ctx, JSValueConst opts,
    size_t* pnjobs, JSValue* ponprogress);

int dyn_ml_progress(JSContext* ctx, JSValueConst onprogress,
    size_t done, size_t total);

JSValue dyn_ml_cross_val_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ml_grid_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int random);

JSValue dyn_ml_impute_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ml_drop_missing(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "core/dyn-serial.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "dyna-io.h"

_Static_assert(offsetof(dyn_linreg_t, fitted) == 0, "dyn_linreg_t: fitted moved");

_Static_assert(offsetof(dyn_logreg_t, fitted) == 0, "dyn_logreg_t: fitted moved");

_Static_assert(offsetof(dyn_kmeans_t, fitted) == 0, "dyn_kmeans_t: fitted moved");

_Static_assert(offsetof(dyn_forest_t, fitted) == 0, "dyn_forest_t: fitted moved");

_Static_assert(offsetof(dyn_pca_t, fitted) == 0, "dyn_pca_t: fitted moved");

_Static_assert(offsetof(dyn_nb_t, fitted) == 0, "dyn_nb_t: fitted moved");

_Static_assert(offsetof(dyn_knn_t, fitted) == 0, "dyn_knn_t: fitted moved");

_Static_assert(offsetof(dyn_scaler_t, fitted) == 0, "dyn_scaler_t: fitted moved");

_Static_assert(offsetof(dyn_svm_t, fitted) == 0, "dyn_svm_t: fitted moved");

_Static_assert(offsetof(dyn_gmm_t, fitted) == 0, "dyn_gmm_t: fitted moved");

int dyn_ml_install_persistence(JSContext* ctx);

extern JSClassID dyn_pipe_class_id;

void dyn_pipe_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

JSValue dyn_pipe_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_pipe_fit(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_pipe_apply(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_pipe_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue dyn_pipe_stage(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

#endif

#endif
