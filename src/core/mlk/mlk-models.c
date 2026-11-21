#include "mlk.h"

void dyn_linreg_dispose(void* native)
{
    dyn_linreg_t* m = (dyn_linreg_t*)native;
    if (m) {
        free(m->coef);
        free(m);
    }
}

void dyn_logreg_dispose(void* native)
{
    dyn_logreg_t* m = (dyn_logreg_t*)native;
    if (!m)
        return;
    free(m->coef);
    free(m->intercept);
    free(m->classes);
    free(m);
}

void dyn_kmeans_dispose(void* native)
{
    dyn_kmeans_t* m = (dyn_kmeans_t*)native;
    if (m) {
        free(m->centroids);
        free(m);
    }
}

void dyn_forest_dispose(void* native)
{
    dyn_forest_t* m = (dyn_forest_t*)native;
    size_t i;
    if (!m)
        return;
    if (m->trees) {
        for (i = 0; i < m->n_trees; i++)
            dyn_tree_free(&m->trees[i]);
        free(m->trees);
    }
    free(m->classes);
    free(m->raw_base);
    free(m);
}

void dyn_pca_dispose(void* native)
{
    dyn_pca_t* m = (dyn_pca_t*)native;
    if (m) {
        free(m->mean);
        free(m->comp);
        free(m->var);
        free(m);
    }
}

void dyn_nb_dispose(void* native)
{
    dyn_nb_t* m = (dyn_nb_t*)native;
    if (m) {
        free(m->classes);
        free(m->prior);
        free(m->mean);
        free(m->var);
        free(m->inv_var);
        free(m->logdet);
        free(m);
    }
}

void dyn_knn_dispose(void* native)
{
    dyn_knn_t* m = (dyn_knn_t*)native;
    if (m) {
        free(m->X);
        free(m->y);
        free(m);
    }
}

void dyn_dbscan_dispose(void* native)
{
    dyn_dbscan_t* m = (dyn_dbscan_t*)native;
    if (m) {
        free(m->labels);
        free(m);
    }
}

void dyn_scaler_dispose(void* native)
{
    dyn_scaler_t* s = (dyn_scaler_t*)native;
    if (s) {
        free(s->centre);
        free(s->scale);
        free(s->spread);
        free(s);
    }
}

void dyn_svm_dispose(void* native)
{
    dyn_svm_t* m = (dyn_svm_t*)native;
    size_t i;
    if (!m)
        return;
    if (m->bin) {
        for (i = 0; i < m->n_bin; i++) {
            free(m->bin[i].alpha);
            free(m->bin[i].sv);
        }
        free(m->bin);
    }
    free(m->classes);
    free(m);
}

void dyn_gmm_dispose(void* native)
{
    dyn_gmm_t* m = (dyn_gmm_t*)native;
    if (m) {
        free(m->weight);
        free(m->mean);
        free(m->var);
        free(m->inv_var);
        free(m->lognorm);
        free(m);
    }
}

int dyn_dbscan_eps_ok(double eps)
{
    return isfinite(eps) && eps > 0.0 && isfinite(eps * eps);
}
