#include "mlk.h"

#define DYN_ML_NODE_BYTES 32u

#define DYN_ML_FITTED_ACCESSOR(T)                  \
    static int dyn_ml_is_fitted_##T(const void* p) \
    {                                              \
        return ((const T*)p)->fitted;              \
    }

static int dyn_ml_fits(const dyn_de_t* r, uint64_t count, size_t elem)
{
    size_t left = dyn_de_left(r);
    return elem == 0 || count <= (uint64_t)(left / elem);
}

static int dyn_ml_de_dims(dyn_de_t* r, uint64_t* out, uint64_t a, uint64_t b)
{
    uint64_t n = a * b;
    if (a && n / a != b) {
        r->err = 1;
        return -1;
    }
    if (!dyn_ml_fits(r, n, sizeof(double))) {
        r->err = 1;
        return -1;
    }
    *out = n;
    return 0;
}

static int dyn_ml_ser_doubles(dyn_ser_t* w, const double* v, size_t n)
{
    return dyn_ser_f64s(w, v, n);
}

static double* dyn_ml_de_doubles(dyn_de_t* r, size_t n)
{
    double* v;
    size_t i;
    if (!dyn_de_ok(r) || !dyn_ml_fits(r, (uint64_t)n, sizeof(double))) {
        r->err = 1;
        return NULL;
    }
    v = (double*)malloc((n ? n : 1) * sizeof(double));
    if (!v)
        return NULL;
    for (i = 0; i < n; i++)
        v[i] = dyn_de_f64(r);
    if (!dyn_de_ok(r)) {
        free(v);
        return NULL;
    }
    return v;
}

static int dyn_ml_w_linear(dyn_ser_t* w, void* native)
{
    dyn_linreg_t* m = (dyn_linreg_t*)native;
    if (dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_f64(w, m->intercept) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->coef, m->n_features);
}

static void* dyn_ml_r_linear(dyn_de_t* r)
{
    dyn_linreg_t* m;
    uint64_t nf = dyn_de_u64(r);
    double intercept = dyn_de_f64(r), *coef;

    if (!dyn_de_ok(r) || !dyn_ml_fits(r, nf, sizeof(double)))
        return NULL;
    coef = dyn_ml_de_doubles(r, (size_t)nf);
    if (!coef)
        return NULL;
    m = (dyn_linreg_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(coef);
        return NULL;
    }
    m->fitted = 1;
    m->n_features = (size_t)nf;
    m->intercept = intercept;
    m->coef = coef;
    return m;
}

static int dyn_ml_w_kmeans(dyn_ser_t* w, void* native)
{
    dyn_kmeans_t* m = (dyn_kmeans_t*)native;
    if (dyn_ser_u64(w, m->k) < 0 || dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_u64(w, m->seed) < 0 || dyn_ser_f64(w, m->inertia) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->centroids, m->k * m->n_features);
}

static void* dyn_ml_r_kmeans(dyn_de_t* r)
{
    dyn_kmeans_t* m;
    uint64_t k = dyn_de_u64(r), nf = dyn_de_u64(r), seed = dyn_de_u64(r), n;
    double inertia = dyn_de_f64(r), *cent;

    if (!dyn_de_ok(r) || k == 0 || dyn_ml_de_dims(r, &n, k, nf) < 0)
        return NULL;
    cent = dyn_ml_de_doubles(r, (size_t)n);
    if (!cent)
        return NULL;
    m = (dyn_kmeans_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(cent);
        return NULL;
    }
    m->fitted = 1;
    m->k = (size_t)k;
    m->n_features = (size_t)nf;
    m->seed = seed;
    m->inertia = inertia;
    m->centroids = cent;
    return m;
}

static int dyn_ml_w_pca(dyn_ser_t* w, void* native)
{
    dyn_pca_t* m = (dyn_pca_t*)native;
    if (dyn_ser_u32(w, (uint32_t)m->whiten) < 0 || dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_u64(w, m->n_components) < 0 || dyn_ser_f64(w, m->total_var) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->mean, m->n_features) < 0 || dyn_ml_ser_doubles(w, m->comp, m->n_components * m->n_features) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->var, m->n_components);
}

static void* dyn_ml_r_pca(dyn_de_t* r)
{
    dyn_pca_t* m;
    uint32_t whiten = dyn_de_u32(r);
    uint64_t nf = dyn_de_u64(r), nc = dyn_de_u64(r), ncomp;
    double total = dyn_de_f64(r), *mean = NULL, *comp = NULL, *var = NULL;

    if (!dyn_de_ok(r) || nf == 0 || nc == 0 || dyn_ml_de_dims(r, &ncomp, nc, nf) < 0)
        return NULL;
    mean = dyn_ml_de_doubles(r, (size_t)nf);
    comp = mean ? dyn_ml_de_doubles(r, (size_t)ncomp) : NULL;
    var = comp ? dyn_ml_de_doubles(r, (size_t)nc) : NULL;
    if (!var) {
        free(mean);
        free(comp);
        return NULL;
    }
    m = (dyn_pca_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(mean);
        free(comp);
        free(var);
        return NULL;
    }
    m->fitted = 1;
    m->whiten = (int)whiten;
    m->n_features = (size_t)nf;
    m->n_components = (size_t)nc;
    m->total_var = total;
    m->mean = mean;
    m->comp = comp;
    m->var = var;
    return m;
}

static int dyn_ml_w_nb(dyn_ser_t* w, void* native)
{
    dyn_nb_t* m = (dyn_nb_t*)native;
    size_t n = m->n_classes * m->n_features;
    if (dyn_ser_f64(w, m->var_smoothing) < 0 || dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_u64(w, m->n_classes) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->classes, m->n_classes) < 0 || dyn_ml_ser_doubles(w, m->prior, m->n_classes) < 0 || dyn_ml_ser_doubles(w, m->mean, n) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->var, n);
}

static void* dyn_ml_r_nb(dyn_de_t* r)
{
    dyn_nb_t* m;
    double vs = dyn_de_f64(r);
    uint64_t nf = dyn_de_u64(r), ncl = dyn_de_u64(r), n;
    double *classes = NULL, *prior = NULL, *mean = NULL, *var = NULL;
    size_t c, j;

    if (!dyn_de_ok(r) || ncl == 0 || dyn_ml_de_dims(r, &n, ncl, nf) < 0)
        return NULL;
    classes = dyn_ml_de_doubles(r, (size_t)ncl);
    prior = classes ? dyn_ml_de_doubles(r, (size_t)ncl) : NULL;
    mean = prior ? dyn_ml_de_doubles(r, (size_t)n) : NULL;
    var = mean ? dyn_ml_de_doubles(r, (size_t)n) : NULL;
    if (!var) {
        free(classes);
        free(prior);
        free(mean);
        return NULL;
    }
    m = (dyn_nb_t*)calloc(1, sizeof(*m));
    if (m) {
        m->inv_var = (double*)malloc((n ? n : 1) * sizeof(double));
        m->logdet = (double*)malloc((ncl ? (size_t)ncl : 1) * sizeof(double));
    }
    if (!m || !m->inv_var || !m->logdet) {
        if (m) {
            free(m->inv_var);
            free(m->logdet);
            free(m);
        }
        free(classes);
        free(prior);
        free(mean);
        free(var);
        return NULL;
    }
    m->fitted = 1;
    m->var_smoothing = vs;
    m->n_features = (size_t)nf;
    m->n_classes = (size_t)ncl;
    m->classes = classes;
    m->prior = prior;
    m->mean = mean;
    m->var = var;
    for (c = 0; c < m->n_classes; c++) {
        double sum = 0;
        for (j = 0; j < m->n_features; j++) {
            double v = m->var[c * m->n_features + j];
            if (!(v > 0.0)) {
                free(m->inv_var);
                free(m->logdet);
                free(m->classes);
                free(m->prior);
                free(m->mean);
                free(m->var);
                free(m);
                return NULL;
            }
            m->inv_var[c * m->n_features + j] = 1.0 / v;
            sum += log(2.0 * M_PI * v);
        }
        m->logdet[c] = sum;
    }
    return m;
}

static int dyn_ml_w_knn(dyn_ser_t* w, void* native)
{
    dyn_knn_t* m = (dyn_knn_t*)native;
    if (dyn_ser_u32(w, (uint32_t)m->regressor) < 0 || dyn_ser_u32(w, (uint32_t)m->weighted) < 0 || dyn_ser_u64(w, m->k) < 0 || dyn_ser_u64(w, m->rows) < 0 || dyn_ser_u64(w, m->cols) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->X, m->rows * m->cols) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->y, m->rows);
}

static void* dyn_ml_r_knn(dyn_de_t* r)
{
    dyn_knn_t* m;
    uint32_t reg = dyn_de_u32(r), wt = dyn_de_u32(r);
    uint64_t k = dyn_de_u64(r), rows = dyn_de_u64(r), cols = dyn_de_u64(r), n;
    double *X = NULL, *y = NULL;

    if (!dyn_de_ok(r) || dyn_ml_de_dims(r, &n, rows, cols) < 0)
        return NULL;
    if (k == 0 || k > rows)
        return NULL;
    X = dyn_ml_de_doubles(r, (size_t)n);
    y = X ? dyn_ml_de_doubles(r, (size_t)rows) : NULL;
    if (!y) {
        free(X);
        return NULL;
    }
    m = (dyn_knn_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(X);
        free(y);
        return NULL;
    }
    m->fitted = 1;
    m->regressor = (int)reg;
    m->weighted = (int)wt;
    m->k = (size_t)k;
    m->rows = (size_t)rows;
    m->cols = (size_t)cols;
    m->X = X;
    m->y = y;
    return m;
}

static int dyn_ml_w_dbscan(dyn_ser_t* w, void* native)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)native;
    size_t i;
    if (dyn_ser_f64(w, m->eps) < 0 || dyn_ser_u64(w, m->min_pts) < 0 || dyn_ser_u64(w, m->rows) < 0 || dyn_ser_u32(w, (uint32_t)m->n_clusters) < 0)
        return -1;
    for (i = 0; i < m->rows; i++)
        if (dyn_ser_u32(w, (uint32_t)m->labels[i]) < 0)
            return -1;
    return 0;
}

static void* dyn_ml_r_dbscan(dyn_de_t* r)
{
    dyn_dbscan_t* m;
    double eps = dyn_de_f64(r);
    uint64_t mp = dyn_de_u64(r), rows = dyn_de_u64(r);
    uint32_t nc = dyn_de_u32(r), i;
    int* labels;

    if (!dyn_de_ok(r) || !dyn_ml_fits(r, rows, 4))
        return NULL;
    if (!dyn_dbscan_eps_ok(eps))
        return NULL;
    if (mp == 0)
        return NULL;
    labels = (int*)malloc((rows ? (size_t)rows : 1) * sizeof(int));
    if (!labels)
        return NULL;
    for (i = 0; i < rows; i++)
        labels[i] = (int)dyn_de_u32(r);
    if (!dyn_de_ok(r)) {
        free(labels);
        return NULL;
    }
    m = (dyn_dbscan_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(labels);
        return NULL;
    }
    m->fitted = 1;
    m->eps = eps;
    m->eps2 = eps * eps;
    m->min_pts = (size_t)mp;
    m->rows = (size_t)rows;
    m->n_clusters = (int)nc;
    m->labels = labels;
    return m;
}

static int dyn_ml_w_scaler(dyn_ser_t* w, void* native)
{
    dyn_scaler_t* s = (dyn_scaler_t*)native;
    if (dyn_ser_u32(w, (uint32_t)s->minmax) < 0 || dyn_ser_u64(w, s->n_features) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, s->centre, s->n_features) < 0 || dyn_ml_ser_doubles(w, s->scale, s->n_features) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, s->spread, s->n_features);
}

static void* dyn_ml_r_scaler(dyn_de_t* r)
{
    dyn_scaler_t* s;
    uint32_t mm = dyn_de_u32(r);
    uint64_t nf = dyn_de_u64(r);
    double *centre = NULL, *scale = NULL, *spread = NULL;

    if (!dyn_de_ok(r) || !dyn_ml_fits(r, nf, 3 * sizeof(double)))
        return NULL;
    centre = dyn_ml_de_doubles(r, (size_t)nf);
    scale = centre ? dyn_ml_de_doubles(r, (size_t)nf) : NULL;
    spread = scale ? dyn_ml_de_doubles(r, (size_t)nf) : NULL;
    if (!spread) {
        free(centre);
        free(scale);
        return NULL;
    }
    s = (dyn_scaler_t*)calloc(1, sizeof(*s));
    if (!s) {
        free(centre);
        free(scale);
        free(spread);
        return NULL;
    }
    s->fitted = 1;
    s->minmax = (int)mm;
    s->n_features = (size_t)nf;
    s->centre = centre;
    s->scale = scale;
    s->spread = spread;
    return s;
}

static int dyn_ml_w_svm(dyn_ser_t* w, void* native)
{
    dyn_svm_t* m = (dyn_svm_t*)native;
    size_t i;
    if (dyn_ser_u32(w, (uint32_t)m->kernel) < 0 || dyn_ser_f64(w, m->gamma) < 0 || dyn_ser_f64(w, m->coef0) < 0 || dyn_ser_u32(w, (uint32_t)m->degree) < 0 || dyn_ser_f64(w, m->C) < 0 || dyn_ser_f64(w, m->tol) < 0 || dyn_ser_u64(w, m->max_iter) < 0 || dyn_ser_u64(w, m->cols) < 0 || dyn_ser_u64(w, m->n_classes) < 0 || dyn_ser_u64(w, m->n_bin) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->classes, m->n_classes) < 0)
        return -1;
    for (i = 0; i < m->n_bin; i++) {
        if (dyn_ser_u64(w, m->bin[i].n_sv) < 0 || dyn_ser_f64(w, m->bin[i].b) < 0)
            return -1;
        if (dyn_ml_ser_doubles(w, m->bin[i].alpha, m->bin[i].n_sv) < 0 || dyn_ml_ser_doubles(w, m->bin[i].sv, m->bin[i].n_sv * m->cols) < 0)
            return -1;
    }
    return 0;
}

static void* dyn_ml_r_svm(dyn_de_t* r)
{
    dyn_svm_t* m;
    uint32_t kernel = dyn_de_u32(r);
    double gamma = dyn_de_f64(r), coef0 = dyn_de_f64(r);
    uint32_t degree = dyn_de_u32(r);
    double C = dyn_de_f64(r), tol = dyn_de_f64(r);
    uint64_t max_iter = dyn_de_u64(r), cols = dyn_de_u64(r);
    uint64_t ncl = dyn_de_u64(r), nbin = dyn_de_u64(r);
    double* classes;
    size_t i;

    if (!dyn_de_ok(r) || kernel > DYN_SVM_POLY || !isfinite(gamma) || !isfinite(coef0) || !isfinite(C) || !isfinite(tol) || ncl < 2 || nbin < 1 || nbin != (ncl == 2 ? 1 : ncl) || degree > 1000 || max_iter > DYN_ML_MAX_ITERS || !dyn_ml_fits(r, ncl, sizeof(double)) || !dyn_ml_fits(r, nbin, sizeof(dyn_svm_bin_t)))
        return NULL;
    classes = dyn_ml_de_doubles(r, (size_t)ncl);
    if (!classes)
        return NULL;
    m = (dyn_svm_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(classes);
        return NULL;
    }
    m->bin = (dyn_svm_bin_t*)calloc(nbin ? (size_t)nbin : 1,
        sizeof(dyn_svm_bin_t));
    if (!m->bin) {
        free(classes);
        free(m);
        return NULL;
    }
    m->fitted = 1;
    m->kernel = (dyn_svm_kernel_t)kernel;
    m->gamma = gamma;
    m->coef0 = coef0;
    m->degree = (int)degree;
    m->C = C;
    m->tol = tol;
    m->max_iter = (size_t)max_iter;
    m->cols = (size_t)cols;
    m->n_classes = (size_t)ncl;
    m->classes = classes;
    m->n_bin = (size_t)nbin;
    for (i = 0; i < m->n_bin; i++) {
        uint64_t nsv = dyn_de_u64(r), n;
        double b = dyn_de_f64(r);
        if (!dyn_de_ok(r) || !isfinite(b) || dyn_ml_de_dims(r, &n, nsv, cols) < 0)
            goto fail;
        m->bin[i].n_sv = (size_t)nsv;
        m->bin[i].b = b;
        m->bin[i].alpha = dyn_ml_de_doubles(r, (size_t)nsv);
        if (!m->bin[i].alpha)
            goto fail;
        m->bin[i].sv = dyn_ml_de_doubles(r, (size_t)n);
        if (!m->bin[i].sv)
            goto fail;
    }
    return m;
fail:
    dyn_svm_dispose(m);
    return NULL;
}

static int dyn_ml_w_gmm(dyn_ser_t* w, void* native)
{
    dyn_gmm_t* m = (dyn_gmm_t*)native;
    if (dyn_ser_u64(w, m->k) < 0 || dyn_ser_u64(w, m->cols) < 0 || dyn_ser_u64(w, m->max_iter) < 0 || dyn_ser_f64(w, m->tol) < 0 || dyn_ser_f64(w, m->reg) < 0 || dyn_ser_u64(w, m->seed) < 0 || dyn_ser_f64(w, m->loglik) < 0 || dyn_ser_u64(w, m->n_iter) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->weight, m->k) < 0 || dyn_ml_ser_doubles(w, m->mean, m->k * m->cols) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->var, m->k * m->cols);
}

static void* dyn_ml_r_gmm(dyn_de_t* r)
{
    dyn_gmm_t* m;
    uint64_t k = dyn_de_u64(r), cols = dyn_de_u64(r), max_iter = dyn_de_u64(r);
    double tol = dyn_de_f64(r), reg = dyn_de_f64(r);
    uint64_t seed = dyn_de_u64(r);
    double loglik = dyn_de_f64(r);
    uint64_t n_iter = dyn_de_u64(r), n;
    double *weight = NULL, *mean = NULL, *var = NULL;
    size_t c, j;

    if (!dyn_de_ok(r) || k == 0 || dyn_ml_de_dims(r, &n, k, cols) < 0)
        return NULL;
    if (max_iter > DYN_ML_MAX_EM_ITERS)
        return NULL;
    weight = dyn_ml_de_doubles(r, (size_t)k);
    mean = weight ? dyn_ml_de_doubles(r, (size_t)n) : NULL;
    var = mean ? dyn_ml_de_doubles(r, (size_t)n) : NULL;
    if (!var) {
        free(weight);
        free(mean);
        return NULL;
    }
    m = (dyn_gmm_t*)calloc(1, sizeof(*m));
    if (m) {
        m->inv_var = (double*)malloc((n ? (size_t)n : 1) * sizeof(double));
        m->lognorm = (double*)malloc((k ? (size_t)k : 1) * sizeof(double));
    }
    if (!m || !m->inv_var || !m->lognorm) {
        if (m) {
            free(m->inv_var);
            free(m->lognorm);
            free(m);
        }
        free(weight);
        free(mean);
        free(var);
        return NULL;
    }
    m->fitted = 1;
    m->k = (size_t)k;
    m->cols = (size_t)cols;
    m->max_iter = (size_t)max_iter;
    m->tol = tol;
    m->reg = reg;
    m->seed = seed;
    m->loglik = loglik;
    m->n_iter = (size_t)n_iter;
    m->weight = weight;
    m->mean = mean;
    m->var = var;
    for (c = 0; c < m->k; c++) {
        double sum = 0;
        for (j = 0; j < m->cols; j++) {
            double v = m->var[c * m->cols + j];
            if (!(v > 0.0))
                v = 1e-12;
            m->inv_var[c * m->cols + j] = 1.0 / v;
            sum += log(2.0 * M_PI * v);
        }
        m->lognorm[c] = log(m->weight[c]) - 0.5 * sum;
    }
    return m;
}

static int dyn_ml_w_logreg(dyn_ser_t* w, void* native)
{
    dyn_logreg_t* m = (dyn_logreg_t*)native;
    if (dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_u64(w, m->n_classes) < 0 || dyn_ser_u64(w, m->n_out) < 0 || dyn_ser_f64(w, m->lr) < 0 || dyn_ser_f64(w, m->l1) < 0 || dyn_ser_f64(w, m->l2) < 0 || dyn_ser_f64(w, m->tol) < 0 || dyn_ser_u64(w, m->max_iter) < 0 || dyn_ser_u32(w, (uint32_t)m->balanced) < 0 || dyn_ser_u64(w, m->n_iter) < 0 || dyn_ser_u32(w, (uint32_t)m->converged) < 0)
        return -1;
    if (dyn_ml_ser_doubles(w, m->classes, m->n_classes) < 0 || dyn_ml_ser_doubles(w, m->intercept, m->n_out) < 0)
        return -1;
    return dyn_ml_ser_doubles(w, m->coef, m->n_out * m->n_features);
}

static void* dyn_ml_r_logreg(dyn_de_t* r)
{
    dyn_logreg_t* m;
    uint64_t nf = dyn_de_u64(r), nc = dyn_de_u64(r), no = dyn_de_u64(r);
    double lr = dyn_de_f64(r), l1 = dyn_de_f64(r), l2 = dyn_de_f64(r);
    double tol = dyn_de_f64(r);
    uint64_t max_iter = dyn_de_u64(r);
    uint32_t balanced = dyn_de_u32(r);
    uint64_t n_iter = dyn_de_u64(r);
    uint32_t converged = dyn_de_u32(r);
    uint64_t ncoef;

    if (!dyn_de_ok(r))
        return NULL;
    if (nc < 2 || no != (nc == 2 ? 1u : nc) || max_iter > DYN_ML_MAX_ITERS)
        return NULL;
    if (nf == 0 || !dyn_ml_fits(r, nc, sizeof(double)))
        return NULL;
    ncoef = no * nf;
    if (nf && ncoef / nf != no)
        return NULL;
    if (!dyn_ml_fits(r, ncoef, sizeof(double)))
        return NULL;

    m = (dyn_logreg_t*)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->classes = dyn_ml_de_doubles(r, (size_t)nc);
    m->intercept = dyn_ml_de_doubles(r, (size_t)no);
    m->coef = dyn_ml_de_doubles(r, (size_t)ncoef);
    if (!m->classes || !m->intercept || !m->coef || !dyn_de_ok(r)) {
        dyn_logreg_dispose(m);
        return NULL;
    }
    m->n_features = (size_t)nf;
    m->n_classes = (size_t)nc;
    m->n_out = (size_t)no;
    m->lr = lr;
    m->l1 = l1;
    m->l2 = l2;
    m->tol = tol;
    m->max_iter = (size_t)max_iter;
    m->balanced = (int)balanced;
    m->n_iter = (size_t)n_iter;
    m->converged = (int)converged;
    m->fitted = 1;
    return m;
}

static int dyn_ml_w_forest(dyn_ser_t* w, void* native)
{
    dyn_forest_t* m = (dyn_forest_t*)native;
    size_t t, i;

    if (dyn_ser_u32(w, (uint32_t)m->classifier) < 0 || dyn_ser_u32(w, (uint32_t)m->boosting) < 0 || dyn_ser_u64(w, m->n_trees) < 0 || dyn_ser_u64(w, m->n_rounds) < 0 || dyn_ser_u64(w, m->n_features) < 0 || dyn_ser_u64(w, m->n_classes) < 0 || dyn_ser_u32(w, m->classes ? 1u : 0u) < 0 || dyn_ser_u64(w, m->n_out) < 0 || dyn_ser_f64(w, m->lr) < 0 || dyn_ser_f64(w, m->subsample) < 0 || dyn_ser_f64(w, m->colsample) < 0 || dyn_ser_u64(w, m->early_stop) < 0 || dyn_ser_f64(w, m->val_frac) < 0 || dyn_ser_u64(w, m->best_rounds) < 0 || dyn_ser_u64(w, m->seed) < 0)
        return -1;
    if (dyn_ser_u64(w, m->opt.max_depth) < 0 || dyn_ser_u64(w, m->opt.min_samples_split) < 0 || dyn_ser_u64(w, m->opt.min_samples_leaf) < 0 || dyn_ser_u64(w, m->opt.max_features) < 0 || dyn_ser_u64(w, m->opt.max_bins) < 0 || dyn_ser_u32(w, (uint32_t)m->opt.newton) < 0 || dyn_ser_f64(w, m->opt.lambda) < 0 || dyn_ser_f64(w, m->opt.alpha) < 0 || dyn_ser_f64(w, m->opt.gamma) < 0 || dyn_ser_f64(w, m->opt.min_child_weight) < 0 || dyn_ser_u64(w, m->opt.seed) < 0)
        return -1;
    if (m->classes && dyn_ml_ser_doubles(w, m->classes, m->n_classes) < 0)
        return -1;
    if (m->n_out && dyn_ml_ser_doubles(w, m->raw_base, m->n_out) < 0)
        return -1;
    for (t = 0; t < m->n_trees; t++) {
        const dyn_tree_t* tr = &m->trees[t];
        if (dyn_ser_u64(w, tr->n_nodes) < 0 || dyn_ser_u64(w, tr->n_classes) < 0)
            return -1;
        for (i = 0; i < tr->n_nodes; i++) {
            const dyn_tnode_t* nd = &tr->node[i];
            if (dyn_ser_u32(w, nd->left) < 0 || dyn_ser_u32(w, nd->right) < 0 || dyn_ser_u32(w, (uint32_t)nd->feature) < 0 || dyn_ser_u32(w, (uint32_t)nd->flags) < 0 || dyn_ser_f64(w, nd->threshold) < 0 || dyn_ser_f64(w, nd->value) < 0)
                return -1;
        }
        if (dyn_ml_ser_doubles(w, tr->improve, tr->n_nodes) < 0 || dyn_ml_ser_doubles(w, tr->nsamp, tr->n_nodes) < 0)
            return -1;
        if (tr->n_classes && dyn_ml_ser_doubles(w, tr->proba, tr->n_nodes * tr->n_classes) < 0)
            return -1;
    }
    return 0;
}

static int dyn_ml_forest_family_ok(uint16_t tid, const dyn_forest_t* m)
{
    int cls = tid == DYN_TID_ML_DTC || tid == DYN_TID_ML_RFC
        || tid == DYN_TID_ML_GBC || tid == DYN_TID_ML_XGBC;
    int boost = tid == DYN_TID_ML_GBR || tid == DYN_TID_ML_GBC
        || tid == DYN_TID_ML_XGBR || tid == DYN_TID_ML_XGBC;
    int newton = tid == DYN_TID_ML_XGBR || tid == DYN_TID_ML_XGBC;
    return !!m->classifier == cls && !!m->boosting == boost
        && !!m->opt.newton == newton;
}

static void* dyn_ml_r_forest(dyn_de_t* r)
{
    dyn_forest_t* m;
    uint32_t classifier = dyn_de_u32(r), boosting = dyn_de_u32(r);
    uint64_t n_trees = dyn_de_u64(r), n_rounds = dyn_de_u64(r);
    uint64_t n_features = dyn_de_u64(r);
    uint64_t n_classes = dyn_de_u64(r);
    uint32_t has_classes = dyn_de_u32(r);
    uint64_t n_out = dyn_de_u64(r);
    double lr = dyn_de_f64(r), subsample = dyn_de_f64(r);
    double colsample = dyn_de_f64(r);
    uint64_t early_stop = dyn_de_u64(r);
    double val_frac = dyn_de_f64(r);
    uint64_t best_rounds = dyn_de_u64(r);
    uint64_t seed = dyn_de_u64(r);
    dyn_tree_opts_t opt;
    double* classes;
    size_t t, i;

    memset(&opt, 0, sizeof(opt));
    opt.max_depth = (size_t)dyn_de_u64(r);
    opt.min_samples_split = (size_t)dyn_de_u64(r);
    opt.min_samples_leaf = (size_t)dyn_de_u64(r);
    opt.max_features = (size_t)dyn_de_u64(r);
    opt.max_bins = (size_t)dyn_de_u64(r);
    opt.newton = (int)dyn_de_u32(r);
    opt.lambda = dyn_de_f64(r);
    opt.alpha = dyn_de_f64(r);
    opt.gamma = dyn_de_f64(r);
    opt.min_child_weight = dyn_de_f64(r);
    opt.seed = dyn_de_u64(r);
    opt.classifier = (int)classifier;

    if (!dyn_de_ok(r) || !dyn_ml_fits(r, n_trees, sizeof(dyn_tree_t)))
        return NULL;
    if (n_features == 0 || !dyn_ml_fits(r, n_features, sizeof(double)))
        return NULL;
    if ((classifier != 0) != (has_classes != 0) || (classifier && n_classes < 2))
        return NULL;
    if (has_classes) {
        if (!dyn_ml_fits(r, n_classes, sizeof(double)))
            return NULL;
        classes = dyn_ml_de_doubles(r, (size_t)n_classes);
        if (!classes)
            return NULL;
    } else {
        classes = NULL;
    }
    if (classifier > 1 || boosting > 1 || (unsigned)opt.newton > 1
        || !(lr >= 0.0 && lr <= 1e12)
        || !(subsample > 0.0 && subsample <= 1.0)
        || !(colsample > 0.0 && colsample <= 1.0)
        || !(val_frac >= 0.0 && val_frac <= 0.5)
        || !(opt.lambda >= 0.0 && opt.lambda <= 1e12)
        || !(opt.alpha >= 0.0 && opt.alpha <= 1e12)
        || !(opt.gamma >= 0.0 && opt.gamma <= 1e12)
        || !(opt.min_child_weight >= 0.0 && opt.min_child_weight <= 1e12)
        || n_rounds > DYN_ML_MAX_TREES
        || (opt.newton && opt.max_bins == 0)) {
        free(classes);
        return NULL;
    }
    if (opt.max_bins > DYN_HIST_MAX_BINS)
        opt.max_bins = DYN_HIST_MAX_BINS;
    if (n_rounds == 0 || (boosting ? 0 : (n_rounds != n_trees))) {
        free(classes);
        return NULL;
    }
    if (boosting) {
        if (n_out == 0 || n_out > n_trees || (n_trees % n_out) != 0) {
            free(classes);
            return NULL;
        }
        if (n_rounds > UINT64_MAX / n_out || n_rounds * n_out != n_trees) {
            if (best_rounds != n_trees / n_out || n_rounds < best_rounds) {
                free(classes);
                return NULL;
            }
            n_rounds = best_rounds;
        }
        if (classifier && n_out != 1 && n_out != n_classes) {
            free(classes);
            return NULL;
        }
    } else if (n_out != 0 || opt.newton) {
        free(classes);
        return NULL;
    }
    m = (dyn_forest_t*)calloc(1, sizeof(*m));
    if (!m) {
        free(classes);
        return NULL;
    }
    m->trees = (dyn_tree_t*)calloc(n_trees ? (size_t)n_trees : 1,
        sizeof(dyn_tree_t));
    if (!m->trees) {
        free(classes);
        free(m);
        return NULL;
    }
    m->fitted = 1;
    m->classifier = (int)classifier;
    m->boosting = (int)boosting;
    m->n_trees = (size_t)n_trees;
    m->n_rounds = (size_t)n_rounds;
    m->n_features = (size_t)n_features;
    m->n_classes = (size_t)n_classes;
    m->classes = classes;
    m->n_out = (size_t)n_out;
    m->lr = lr;
    m->subsample = subsample;
    m->colsample = colsample;
    m->early_stop = (size_t)early_stop;
    m->val_frac = val_frac;
    m->best_rounds = (size_t)best_rounds;
    m->seed = seed;
    m->opt = opt;
    if (n_out) {
        if (!dyn_ml_fits(r, n_out, sizeof(double)))
            goto fail;
        m->raw_base = dyn_ml_de_doubles(r, (size_t)n_out);
        if (!m->raw_base)
            goto fail;
        for (i = 0; i < (size_t)n_out; i++)
            if (!isfinite(m->raw_base[i]))
                goto fail;
    }
    for (t = 0; t < m->n_trees; t++) {
        uint64_t nn = dyn_de_u64(r), tk = dyn_de_u64(r), pn;
        dyn_tnode_t* nodes;
        if (!dyn_de_ok(r) || nn == 0 || !dyn_ml_fits(r, nn, DYN_ML_NODE_BYTES))
            goto fail;
        if (tk != 0 && tk != n_classes)
            goto fail;
        nodes = (dyn_tnode_t*)malloc((nn ? (size_t)nn : 1) * sizeof(*nodes));
        if (!nodes)
            goto fail;
        m->trees[t].node = nodes;
        m->trees[t].n_nodes = m->trees[t].cap = (size_t)nn;
        m->trees[t].n_classes = (size_t)tk;
        (void)pn;
        for (i = 0; i < nn; i++) {
            nodes[i].left = dyn_de_u32(r);
            nodes[i].right = dyn_de_u32(r);
            nodes[i].feature = (int32_t)dyn_de_u32(r);
            nodes[i].flags = (int32_t)dyn_de_u32(r);
            nodes[i].threshold = dyn_de_f64(r);
            nodes[i].value = dyn_de_f64(r);
            if (!isfinite(nodes[i].value))
                goto fail;
            if (!isfinite(nodes[i].threshold)
                && !(nodes[i].feature >= 0 && nodes[i].threshold < 0.0))
                goto fail;
            if (nodes[i].feature < 0) {
                if (nodes[i].left != DYN_TREE_LEAF || nodes[i].right != DYN_TREE_LEAF)
                    goto fail;
            } else {
                if ((uint64_t)nodes[i].feature >= n_features || nodes[i].left >= nn || nodes[i].left <= i || nodes[i].right >= nn || nodes[i].right <= i)
                    goto fail;
            }
        }
        if (!dyn_de_ok(r))
            goto fail;
        m->trees[t].improve = dyn_ml_de_doubles(r, (size_t)nn);
        if (!m->trees[t].improve)
            goto fail;
        m->trees[t].nsamp = dyn_ml_de_doubles(r, (size_t)nn);
        if (!m->trees[t].nsamp)
            goto fail;
        if (tk) {
            pn = nn * tk;
            if (nn && pn / nn != tk)
                goto fail;
            m->trees[t].proba = dyn_ml_de_doubles(r, (size_t)pn);
            if (!m->trees[t].proba)
                goto fail;
        }
    }
    return m;
fail:
    dyn_forest_dispose(m);
    return NULL;
}

DYN_ML_FITTED_ACCESSOR(dyn_linreg_t)

DYN_ML_FITTED_ACCESSOR(dyn_logreg_t)

DYN_ML_FITTED_ACCESSOR(dyn_kmeans_t)

DYN_ML_FITTED_ACCESSOR(dyn_forest_t)

DYN_ML_FITTED_ACCESSOR(dyn_pca_t)

DYN_ML_FITTED_ACCESSOR(dyn_nb_t)

DYN_ML_FITTED_ACCESSOR(dyn_knn_t)

DYN_ML_FITTED_ACCESSOR(dyn_dbscan_t)

DYN_ML_FITTED_ACCESSOR(dyn_scaler_t)

DYN_ML_FITTED_ACCESSOR(dyn_svm_t)

DYN_ML_FITTED_ACCESSOR(dyn_gmm_t)

const dyn_mlk_codec_t dyn_mlk_codecs[DYN_MLK_N_CODECS] = {
    [DYN_MLK_C_LINREG] = { DYN_TID_ML_LINREG, "LinearRegression",
        dyn_ml_w_linear, dyn_ml_r_linear, dyn_linreg_dispose,
        dyn_ml_is_fitted_dyn_linreg_t },
    [DYN_MLK_C_LOGREG] = { DYN_TID_ML_LOGREG, "LogisticRegression",
        dyn_ml_w_logreg, dyn_ml_r_logreg, dyn_logreg_dispose,
        dyn_ml_is_fitted_dyn_logreg_t },
    [DYN_MLK_C_KMEANS] = { DYN_TID_ML_KMEANS, "KMeans",
        dyn_ml_w_kmeans, dyn_ml_r_kmeans, dyn_kmeans_dispose,
        dyn_ml_is_fitted_dyn_kmeans_t },
    [DYN_MLK_C_PCA] = { DYN_TID_ML_PCA, "PCA",
        dyn_ml_w_pca, dyn_ml_r_pca, dyn_pca_dispose,
        dyn_ml_is_fitted_dyn_pca_t },
    [DYN_MLK_C_NB] = { DYN_TID_ML_NB, "GaussianNB",
        dyn_ml_w_nb, dyn_ml_r_nb, dyn_nb_dispose,
        dyn_ml_is_fitted_dyn_nb_t },
    [DYN_MLK_C_KNN_CLF] = { DYN_TID_ML_KNN_CLF, "KNClassifier",
        dyn_ml_w_knn, dyn_ml_r_knn, dyn_knn_dispose,
        dyn_ml_is_fitted_dyn_knn_t },
    [DYN_MLK_C_KNN_REG] = { DYN_TID_ML_KNN_REG, "KNRegressor",
        dyn_ml_w_knn, dyn_ml_r_knn, dyn_knn_dispose,
        dyn_ml_is_fitted_dyn_knn_t },
    [DYN_MLK_C_DBSCAN] = { DYN_TID_ML_DBSCAN, "DBScan",
        dyn_ml_w_dbscan, dyn_ml_r_dbscan, dyn_dbscan_dispose,
        dyn_ml_is_fitted_dyn_dbscan_t },
    [DYN_MLK_C_STDSCALER] = { DYN_TID_ML_STDSCALER, "StandardScaler",
        dyn_ml_w_scaler, dyn_ml_r_scaler, dyn_scaler_dispose,
        dyn_ml_is_fitted_dyn_scaler_t },
    [DYN_MLK_C_MINMAX] = { DYN_TID_ML_MINMAX, "MinMaxScaler",
        dyn_ml_w_scaler, dyn_ml_r_scaler, dyn_scaler_dispose,
        dyn_ml_is_fitted_dyn_scaler_t },
    [DYN_MLK_C_SVC] = { DYN_TID_ML_SVC, "SVC",
        dyn_ml_w_svm, dyn_ml_r_svm, dyn_svm_dispose,
        dyn_ml_is_fitted_dyn_svm_t },
    [DYN_MLK_C_GMM] = { DYN_TID_ML_GMM, "GaussianMixture",
        dyn_ml_w_gmm, dyn_ml_r_gmm, dyn_gmm_dispose,
        dyn_ml_is_fitted_dyn_gmm_t },
    [DYN_MLK_C_DTC] = { DYN_TID_ML_DTC, "DecisionTreeClassifier",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_DTR] = { DYN_TID_ML_DTR, "DecisionTreeRegressor",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_RFC] = { DYN_TID_ML_RFC, "RandomForestClassifier",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_RFR] = { DYN_TID_ML_RFR, "RandomForestRegressor",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_GBR] = { DYN_TID_ML_GBR, "GradientBoostingRegressor",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_GBC] = { DYN_TID_ML_GBC, "GradientBoostingClassifier",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_XGBR] = { DYN_TID_ML_XGBR, "XGBRegressor",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
    [DYN_MLK_C_XGBC] = { DYN_TID_ML_XGBC, "XGBClassifier",
        dyn_ml_w_forest, dyn_ml_r_forest, dyn_forest_dispose,
        dyn_ml_is_fitted_dyn_forest_t },
};

uint8_t* dyn_mlk_encode(const dyn_mlk_codec_t* c, void* native, size_t* len)
{
    dyn_ser_t w;
    dyn_ser_init(&w);
    if (dyn_ser_begin(&w, c->type_id, 0) < 0 || c->write(&w, native) < 0 || dyn_ser_finish(&w) < 0) {
        dyn_ser_free(&w);
        return NULL;
    }
    return dyn_ser_take(&w, len);
}

int dyn_mlk_decode(const dyn_mlk_codec_t* c, const uint8_t* p, size_t len, void** out, int* de_rc, const char** held)
{
    dyn_de_t r;
    uint16_t tid = 0;
    void* native;
    int rc = dyn_de_open(&r, p, len, &tid, NULL, 0);

    *out = NULL;
    *held = NULL;
    *de_rc = rc;
    if (rc != DYN_DE_OK)
        return DYN_MLK_DEC_FORMAT;
    if (tid != c->type_id) {
        size_t i;
        for (i = 0; i < DYN_MLK_N_CODECS; i++)
            if (dyn_mlk_codecs[i].type_id == tid) {
                *held = dyn_mlk_codecs[i].name;
                break;
            }
        return DYN_MLK_DEC_WRONG_TYPE;
    }
    native = c->read(&r);
    if (!native)
        return DYN_MLK_DEC_MALFORMED;
    if (!dyn_de_ok(&r) || dyn_de_left(&r) != 0
        || (c->read == dyn_ml_r_forest
            && !dyn_ml_forest_family_ok(tid, (const dyn_forest_t*)native))) {
        c->dispose(native);
        return DYN_MLK_DEC_MALFORMED;
    }
    *out = native;
    return DYN_MLK_DEC_OK;
}
