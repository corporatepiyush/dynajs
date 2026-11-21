#ifndef DYN_MLK_MLK_H
#define DYN_MLK_MLK_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-cdefs.h"
#include "../dyn-prng.h"
#include "../dyn-serial.h"

#define DYN_ML_MAX_ITERS 100000
#define DYN_ML_MAX_EM_ITERS 10000
#define DYN_ML_MAX_TREES 100000

#define DYN_TREE_MAX_DEPTH 1024

static inline double dyn_ml_dot(const double* a, const double* b, size_t n)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        s0 += a[j] * b[j];
        s1 += a[j + 1] * b[j + 1];
        s2 += a[j + 2] * b[j + 2];
        s3 += a[j + 3] * b[j + 3];
    }
#endif
    for (; j < n; j++)
        s0 += a[j] * b[j];
    return (s0 + s1) + (s2 + s3);
}

static inline double dyn_ml_sqdist(const double* a, const double* b, size_t d)
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, t;
    size_t j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= d; j += 4) {
        t = a[j] - b[j];
        s0 += t * t;
        t = a[j + 1] - b[j + 1];
        s1 += t * t;
        t = a[j + 2] - b[j + 2];
        s2 += t * t;
        t = a[j + 3] - b[j + 3];
        s3 += t * t;
    }
#endif
    for (; j < d; j++) {
        t = a[j] - b[j];
        s0 += t * t;
    }
    return (s0 + s1) + (s2 + s3);
}

static inline void dyn_ml_axpy(double* restrict y, double alpha,
    const double* restrict x, size_t n)
{
    size_t j;
    for (j = 0; j < n; j++) {
        double p = alpha * x[j];
        y[j] = y[j] + p;
    }
}

int dyn_solve(double* A, double* b, size_t p, double min_pivot);

typedef int (*dyn_ml_cmp_fn)(const void*, const void*);

void dyn_ml_isort(void* base, size_t n, size_t w, dyn_ml_cmp_fn cmp);

typedef struct {
    double* val;
    uint32_t* col;
    size_t* ptr;
    size_t rows, cols, nnz;
} dyn_csr_t;

double dyn_csr_dot(const double* val, const uint32_t* col, size_t nz,
    const double* dense);

void dyn_csr_axpy(double* dense, double a, const double* val,
    const uint32_t* col, size_t nz);

typedef struct {
    uint32_t col;
    double val;
    size_t pos;
} dyn_csr_ent_t;

size_t dyn_csr_max_row(const dyn_csr_t* S);

long dyn_csr_row_reading(const dyn_csr_t* S, size_t i, dyn_csr_ent_t* ent,
    int* bad_nan);

#define DYN_TREE_LEAF ((uint32_t)0xffffffffu)

typedef struct {
    uint32_t left, right;
    int32_t feature;
    int32_t flags;
    double threshold;
    double value;
} dyn_tnode_t;

typedef struct {
    dyn_tnode_t* node;
    size_t n_nodes, cap;
    double* improve;
    double* nsamp;
    double* proba;
    size_t n_classes;
} dyn_tree_t;

typedef struct {
    size_t max_depth;
    size_t min_samples_split;
    size_t min_samples_leaf;
    size_t max_features;
    size_t max_bins;
    int classifier;
    int newton;
    double lambda;
    double alpha;
    double gamma;
    double min_child_weight;
    uint64_t seed;
} dyn_tree_opts_t;

void dyn_tree_free(dyn_tree_t* t);

typedef struct {
    double key;
    uint32_t row;
    uint32_t pad;
} dyn_tree_kv_t;

int dyn_ml_kv_cmp(const void* a, const void* b);
int dyn_ml_kv_radix(dyn_tree_kv_t* a, size_t n);

void dyn_ml_kv_sort_r(dyn_tree_kv_t* a, size_t n, size_t depth,
    size_t limit);

#define DYN_HIST_MAX_BINS 255u

typedef struct {
    uint8_t* code;
    double *vmin, *vmax;
    uint32_t* nbin;
    size_t max_bins, rows, cols;
    int exact;
} dyn_hist_t;

void dyn_hist_free(dyn_hist_t* h);

int dyn_hist_build(dyn_hist_t* h, const double* X, size_t rows,
    size_t cols, size_t max_bins, dyn_tree_kv_t* kv,
    double* vals, const double* w);

double dyn_tree_predict_row(const dyn_tree_t* t, const double* x);

double dyn_tree_predict_row_missing(const dyn_tree_t* t, const double* x);

typedef struct {
    uint32_t* scratch;
    uint32_t* feat;
    double *cnt_l, *cnt_r, *cnt_tot;
    dyn_tree_kv_t* kv;
    uint32_t* best_order;
    const dyn_hist_t* hist;
    double* hbuf;
    uint32_t* hnext;
    size_t hstride;
    const double *grad, *hess;
    const double* w;
    const uint8_t* cls_of;
    size_t n_cand;
    uint8_t* tiefree;
} dyn_tree_work_t;

uint32_t dyn_tree_leaf_index(const dyn_tree_t* t, const double* x,
    size_t cols);

uint32_t dyn_tree_leaf_index_missing(const dyn_tree_t* t, const double* x);

void dyn_boost_leaf_newton(dyn_tree_t* t, const double* X, size_t cols,
    const double* grad, const double* hess,
    const double* w, const uint32_t* rows_in,
    size_t n, double scale, double* num,
    double* den);

void dyn_boost_init_base(int classifier, size_t n_out, const double* y,
    size_t rows, const double* classes,
    const double* w, double* raw_base, double* pred);

void dyn_xgb_grad_hess(int classifier, size_t n_out, size_t k,
    const double* pred, const double* y,
    const double* classes, const double* w,
    size_t rows, double* g, double* h);

double dyn_xgb_val_loss(int classifier, size_t n_out, const double* pred,
    const double* y, const double* classes,
    const uint8_t* isval, size_t rows);

#define DYN_JACOBI_SWEEPS 60

void dyn_jacobi(double* A, double* V, size_t p);

int dyn_tree_fit(dyn_tree_t* t, const double* X, const double* y, size_t rows, size_t cols,
    const double* classes, size_t n_classes, const dyn_tree_opts_t* opt, uint64_t* rng,
    uint32_t* idx, size_t n, const dyn_tree_work_t* w, int (*poll)(void*), void* poll_ud,
    int* interrupted);

typedef struct {
    int fitted;
    size_t n_features;
    double* coef;
    double intercept;
} dyn_linreg_t;

typedef struct {
    int fitted;
    size_t n_features;
    size_t n_classes;
    size_t n_out;
    double* classes;
    double* coef;
    double* intercept;
    double lr, l1, l2, tol;
    size_t max_iter;
    int balanced;
    double C;
    int have_C;
    size_t n_iter;
    int converged;
} dyn_logreg_t;

typedef struct {
    int fitted;
    size_t k;
    size_t n_features;
    uint64_t seed;
    double* centroids;
    double inertia;
} dyn_kmeans_t;

typedef struct {
    int fitted;
    int classifier;
    int boosting;
    size_t n_trees;
    size_t n_rounds;
    size_t n_features;
    size_t n_classes;
    double* classes;
    double* raw_base;
    size_t n_out;
    double lr;
    double subsample;
    double colsample;
    size_t early_stop;
    double val_frac;
    size_t best_rounds;
    dyn_tree_t* trees;
    dyn_tree_opts_t opt;
    uint64_t seed;
} dyn_forest_t;

typedef struct {
    int fitted;
    int whiten;
    size_t n_features;
    size_t n_components;
    double* mean;
    double* comp;
    double* var;
    double total_var;
} dyn_pca_t;

typedef struct {
    int fitted;
    double var_smoothing;
    size_t n_features;
    size_t n_classes;
    double* classes;
    double* prior;
    double* mean;
    double* var;
    double* inv_var;
    double* logdet;
} dyn_nb_t;

typedef struct {
    int fitted;
    int regressor;
    int weighted;
    size_t k;
    size_t rows, cols;
    double* X;
    double* y;
} dyn_knn_t;

typedef struct {
    double eps2;
    double eps;
    size_t min_pts;
    size_t rows;
    int* labels;
    int fitted;
    int n_clusters;
} dyn_dbscan_t;

typedef struct {
    int fitted;
    int minmax;
    size_t n_features;
    double* centre;
    double* scale;
    double* spread;
} dyn_scaler_t;

typedef enum { DYN_SVM_LINEAR,
    DYN_SVM_RBF,
    DYN_SVM_POLY } dyn_svm_kernel_t;

typedef struct {
    double* alpha;
    double* sv;
    double b;
    size_t n_sv;
} dyn_svm_bin_t;

typedef struct {
    int fitted;
    dyn_svm_kernel_t kernel;
    double gamma;
    double coef0;
    int degree;
    double C;
    double tol;
    size_t max_iter;
    size_t cols;
    size_t n_classes;
    double* classes;
    dyn_svm_bin_t* bin;
    size_t n_bin;
} dyn_svm_t;

typedef struct {
    int fitted;
    size_t k;
    size_t cols;
    size_t max_iter;
    double tol;
    double reg;
    uint64_t seed;
    double* weight;
    double* mean;
    double* var;
    double* inv_var;
    double* lognorm;
    double loglik;
    size_t n_iter;
} dyn_gmm_t;

enum { MLK_CL_OK = 0,
    MLK_CL_E_FEW_ROWS = -1,
    MLK_CL_E_K_CLOSE = -2,
    MLK_CL_E_NOMEM = -3,
    MLK_CL_E_INTR = -4,
    MLK_CL_E_TOO_BIG = -5 };

double dyn_km_assign(const double* X, size_t rows, size_t cols,
    const double* cent, size_t k, int* labels,
    const double* w);

void dyn_km_plusplus(const double* X, size_t rows, size_t cols,
    size_t k, double* cent, double* nearest,
    uint64_t* rng);

#define DYN_DBSCAN_MAX_GRID_COLS 6
#define DYN_DBSCAN_MAX_GRID_CELLS 729

_Static_assert(3 * 3 * 3 * 3 * 3 * 3 == DYN_DBSCAN_MAX_GRID_CELLS,
    "DYN_DBSCAN_MAX_GRID_CELLS must be 3^DYN_DBSCAN_MAX_GRID_COLS");

int mlk_kmeans_train(dyn_kmeans_t* m, const double* X,
    size_t rows, size_t cols, const double* w, int (*poll)(void*), void* ud);

int mlk_dbscan_run(dyn_dbscan_t* m, const double* X,
    size_t rows, size_t cols, int* labels);

double dyn_gmm_logpdf(const dyn_gmm_t* m, const double* x, size_t c);

double dyn_gmm_estep(const dyn_gmm_t* m, const double* X, size_t rows,
    double* resp);

int mlk_gmm_learn(dyn_gmm_t* m, const double* X,
    size_t rows, size_t cols, int (*poll)(void*), void* ud);

void dyn_linreg_dispose(void* native);

void dyn_logreg_dispose(void* native);

void dyn_kmeans_dispose(void* native);

void dyn_forest_dispose(void* native);

void dyn_pca_dispose(void* native);

void dyn_nb_dispose(void* native);

void dyn_knn_dispose(void* native);

void dyn_dbscan_dispose(void* native);

int dyn_dbscan_eps_ok(double eps);

void dyn_scaler_dispose(void* native);

void dyn_svm_dispose(void* native);

void dyn_gmm_dispose(void* native);

#define DYN_TID_ML_LINREG 100
#define DYN_TID_ML_LOGREG 101
#define DYN_TID_ML_KMEANS 102
#define DYN_TID_ML_PCA 103
#define DYN_TID_ML_NB 104
#define DYN_TID_ML_KNN_CLF 105
#define DYN_TID_ML_KNN_REG 106
#define DYN_TID_ML_DBSCAN 107
#define DYN_TID_ML_STDSCALER 108
#define DYN_TID_ML_MINMAX 109
#define DYN_TID_ML_SVC 110
#define DYN_TID_ML_GMM 111
#define DYN_TID_ML_DTC 112
#define DYN_TID_ML_DTR 113
#define DYN_TID_ML_RFC 114
#define DYN_TID_ML_RFR 115
#define DYN_TID_ML_GBR 116
#define DYN_TID_ML_GBC 117
#define DYN_TID_ML_XGBR 118
#define DYN_TID_ML_XGBC 119

enum {
    DYN_MLK_C_LINREG,
    DYN_MLK_C_LOGREG,
    DYN_MLK_C_KMEANS,
    DYN_MLK_C_PCA,
    DYN_MLK_C_NB,
    DYN_MLK_C_KNN_CLF,
    DYN_MLK_C_KNN_REG,
    DYN_MLK_C_DBSCAN,
    DYN_MLK_C_STDSCALER,
    DYN_MLK_C_MINMAX,
    DYN_MLK_C_SVC,
    DYN_MLK_C_GMM,
    DYN_MLK_C_DTC,
    DYN_MLK_C_DTR,
    DYN_MLK_C_RFC,
    DYN_MLK_C_RFR,
    DYN_MLK_C_GBR,
    DYN_MLK_C_GBC,
    DYN_MLK_C_XGBR,
    DYN_MLK_C_XGBC,
    DYN_MLK_N_CODECS
};

typedef struct {
    uint16_t type_id;
    const char* name;
    int (*write)(dyn_ser_t* w, void* native);
    void* (*read)(dyn_de_t* r);
    void (*dispose)(void* native);
    int (*is_fitted)(const void* native);
} dyn_mlk_codec_t;

extern const dyn_mlk_codec_t dyn_mlk_codecs[DYN_MLK_N_CODECS];

enum {
    DYN_MLK_DEC_OK,
    DYN_MLK_DEC_FORMAT,
    DYN_MLK_DEC_WRONG_TYPE,
    DYN_MLK_DEC_MALFORMED
};

uint8_t* dyn_mlk_encode(const dyn_mlk_codec_t* c, void* native, size_t* len);

int dyn_mlk_decode(const dyn_mlk_codec_t* c, const uint8_t* p, size_t len, void** out, int* de_rc, const char** held);

#endif
