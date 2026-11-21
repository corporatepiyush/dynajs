#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mlk.h"
#include "../dyn-prng.h"

#define DYN_KMEANS_MAX_ITER 300

static inline void dyn_ml_scale(double* restrict out,
    const double* restrict x, double s, size_t n)
{
    size_t j;
    for (j = 0; j < n; j++)
        out[j] = x[j] * s;
}

typedef struct {
    size_t* head;
    size_t* next;
    uint32_t* mgen;
    size_t* mbkt;
    uint32_t gen;
    size_t slot_mask;
    size_t cap;
    size_t rows;
    size_t cols;
    double inv_eps;
} dyn_dbscan_grid_t;

double dyn_km_assign(const double* X, size_t rows, size_t cols,
    const double* cent, size_t k, int* labels,
    const double* w)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
        double best = dyn_ml_sqdist(xi, cent, cols);
        int bl = 0;
        for (c = 1; c < k; c++) {
            double d = dyn_ml_sqdist(xi, cent + c * cols, cols);
            if (d < best) {
                best = d;
                bl = (int)c;
            }
        }
        if (labels)
            labels[i] = bl;
        total += w ? w[i] * best : best;
    }
    return total;
}

void dyn_km_plusplus(const double* X, size_t rows, size_t cols,
    size_t k, double* cent, double* nearest,
    uint64_t* rng)
{
    size_t c, i;
    size_t first = (size_t)(dyn_splitmix64(rng) % rows);
    memcpy(cent, X + first * cols, cols * sizeof(double));
    for (i = 0; i < rows; i++)
        nearest[i] = dyn_ml_sqdist(X + i * cols, cent, cols);
    for (c = 1; c < k; c++) {
        double sum = 0.0, target;
        size_t chosen = 0;
        for (i = 0; i < rows; i++)
            sum += nearest[i];
        if (sum <= 0.0) {
            chosen = (size_t)(dyn_splitmix64(rng) % rows);
        } else {
            target = ((double)(dyn_splitmix64(rng) >> 11) * (1.0 / 9007199254740992.0)) * sum;
            for (i = 0; i < rows; i++) {
                target -= nearest[i];
                if (target <= 0.0) {
                    chosen = i;
                    break;
                }
                chosen = i;
            }
        }
        memcpy(cent + c * cols, X + chosen * cols, cols * sizeof(double));
        for (i = 0; i < rows; i++) {
            double d = dyn_ml_sqdist(X + i * cols, cent + c * cols, cols);
            if (d < nearest[i])
                nearest[i] = d;
        }
    }
}

int mlk_kmeans_train(dyn_kmeans_t* m, const double* X,
    size_t rows, size_t cols, const double* w, int (*poll)(void*), void* ud)
{
    size_t k = m->k, i, c, it;
    double *cent = NULL, *sums = NULL, *nearest = NULL;
    int *labels = NULL, *prev = NULL;
    double* counts = NULL;
    double last = 0.0;
    uint64_t rng = m->seed;

    if (rows < k)
        return MLK_CL_E_FEW_ROWS;
    if (k > 64 && k > rows / 2)
        return MLK_CL_E_K_CLOSE;
    cent = (double*)malloc(k * cols * sizeof(double));
    sums = (double*)malloc(k * cols * sizeof(double));
    nearest = (double*)malloc(rows * sizeof(double));
    labels = (int*)malloc(rows * sizeof(int));
    prev = (int*)malloc(rows * sizeof(int));
    counts = (double*)malloc(k * sizeof(double));
    if (!cent || !sums || !nearest || !labels || !prev || !counts) {
        free(cent);
        free(sums);
        free(nearest);
        free(labels);
        free(prev);
        free(counts);
        return MLK_CL_E_NOMEM;
    }
    dyn_km_plusplus(X, rows, cols, k, cent, nearest, &rng);
    for (i = 0; i < rows; i++)
        prev[i] = -1;
    for (it = 0; it < DYN_KMEANS_MAX_ITER; it++) {
        int changed = 0;
        if (poll(ud)) {
            free(cent);
            free(sums);
            free(nearest);
            free(labels);
            free(prev);
            free(counts);
            return MLK_CL_E_INTR;
        }
        last = dyn_km_assign(X, rows, cols, cent, k, labels, w);
        for (i = 0; i < rows; i++)
            if (labels[i] != prev[i]) {
                changed = 1;
                break;
            }
        if (!changed && it > 0)
            break;
        memset(sums, 0, k * cols * sizeof(double));
        for (c = 0; c < k; c++)
            counts[c] = 0.0;
        if (w) {
            for (i = 0; i < rows; i++) {
                size_t lb = (size_t)labels[i];
                counts[lb] += w[i];
                dyn_ml_axpy(sums + lb * cols, w[i], X + i * cols, cols);
            }
        } else {
            for (i = 0; i < rows; i++) {
                size_t lb = (size_t)labels[i];
                counts[lb] += 1.0;
                dyn_ml_axpy(sums + lb * cols, 1.0, X + i * cols, cols);
            }
        }
        for (c = 0; c < k; c++) {
            if (counts[c] <= 0.0) {
                size_t r = (size_t)(dyn_splitmix64(&rng) % rows);
                memcpy(cent + c * cols, X + r * cols, cols * sizeof(double));
            } else {
                dyn_ml_scale(cent + c * cols, sums + c * cols,
                    1.0 / counts[c], cols);
            }
        }
        memcpy(prev, labels, rows * sizeof(int));
    }
    m->inertia = (it == DYN_KMEANS_MAX_ITER)
        ? dyn_km_assign(X, rows, cols, cent, k, labels, w)
        : last;
    free(m->centroids);
    m->centroids = cent;
    m->n_features = cols;
    m->fitted = 1;
    free(sums);
    free(nearest);
    free(labels);
    free(prev);
    free(counts);
    return 0;
}

static uint64_t dyn_dbscan_cell_hash(const int64_t* c, size_t cols)
{
    uint64_t h = 1469598103934665603ULL;
    size_t d;
    for (d = 0; d < cols; d++) {
        h ^= (uint64_t)c[d];
        h *= 1099511628211ULL;
    }
    return h;
}

static int64_t dyn_dbscan_coord(double x, double inv_eps)
{
    double cd = floor(x * inv_eps);
    if (isnan(cd))
        cd = 0.0;
    else if (cd > 8.0e18)
        cd = 8.0e18;
    else if (cd < -8.0e18)
        cd = -8.0e18;
    return (int64_t)cd;
}

static int dyn_dbscan_grid_build(dyn_dbscan_grid_t* g,
    const double* X, size_t rows, size_t cols,
    double eps)
{
    size_t cap = 16, i, d, cells = 1, slots = 16;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];

    while (cap < rows * 2 && cap <= SIZE_MAX / 2)
        cap *= 2;
    for (d = 0; d < cols; d++)
        cells *= 3;
    while (slots < cells * 2)
        slots *= 2;
    g->head = (size_t*)malloc(cap * sizeof(size_t));
    g->next = (size_t*)malloc(rows * sizeof(size_t));
    g->mgen = (uint32_t*)calloc(slots, sizeof(uint32_t));
    g->mbkt = (size_t*)malloc(slots * sizeof(size_t));
    if (!g->head || !g->next || !g->mgen || !g->mbkt) {
        free(g->head);
        free(g->next);
        free(g->mgen);
        free(g->mbkt);
        g->head = NULL;
        g->next = NULL;
        g->mgen = NULL;
        g->mbkt = NULL;
        return -1;
    }
    for (i = 0; i < cap; i++)
        g->head[i] = SIZE_MAX;
    g->cap = cap;
    g->rows = rows;
    g->gen = 1;
    g->slot_mask = slots - 1;
    g->cols = cols;
    g->inv_eps = 1.0 / eps;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * cols;
        for (d = 0; d < cols; d++)
            c[d] = dyn_dbscan_coord(xi[d], g->inv_eps);
        {
            size_t b = (size_t)dyn_dbscan_cell_hash(c, cols) & (cap - 1);
            g->next[i] = g->head[b];
            g->head[b] = i;
        }
    }
    return 0;
}

static size_t dyn_dbscan_grid_region(dyn_dbscan_grid_t* g,
    const double* X, size_t p, double eps2,
    size_t* out)
{
    const double* xp = X + p * g->cols;
    int64_t c[DYN_DBSCAN_MAX_GRID_COLS];
    int64_t off[DYN_DBSCAN_MAX_GRID_COLS] = { 0 };
    size_t n = 0, d, k, cap = g->rows;
    uint32_t gen;

    if (++g->gen == 0) {
        memset(g->mgen, 0, (g->slot_mask + 1) * sizeof(uint32_t));
        g->gen = 1;
    }
    gen = g->gen;
    for (d = 0; d < g->cols; d++)
        c[d] = dyn_dbscan_coord(xp[d], g->inv_eps);
    for (;;) {
        int64_t cc[DYN_DBSCAN_MAX_GRID_COLS];
        for (d = 0; d < g->cols; d++)
            cc[d] = c[d] + off[d] - 1;
        {
            size_t b = (size_t)dyn_dbscan_cell_hash(cc, g->cols) & (g->cap - 1);
            size_t hd = g->head[b];
            if (hd != SIZE_MAX) {
                int claimed = 0;
                for (k = hd; k != SIZE_MAX; k = g->next[k]) {
                    if (dyn_ml_sqdist(xp, X + k * g->cols, g->cols) > eps2)
                        continue;
                    if (!claimed) {
                        size_t s = b & g->slot_mask;
                        while (g->mgen[s] == gen && g->mbkt[s] != b)
                            s = (s + 1) & g->slot_mask;
                        if (g->mgen[s] == gen)
                            break;
                        g->mgen[s] = gen;
                        g->mbkt[s] = b;
                        claimed = 1;
                    }
                    if (n >= cap)
                        break;
                    out[n++] = k;
                }
            }
        }
        for (d = 0; d < g->cols; d++) {
            if (++off[d] < 3)
                break;
            off[d] = 0;
        }
        if (d == g->cols)
            break;
    }
    return n;
}

static size_t dyn_dbscan_region(const double* X, size_t rows, size_t cols,
    size_t p, double eps2, size_t* out)
{
    const double* xp = X + p * cols;
    size_t n = 0, i;
    for (i = 0; i < rows; i++)
        if (dyn_ml_sqdist(xp, X + i * cols, cols) <= eps2)
            out[n++] = i;
    return n;
}

int mlk_dbscan_run(dyn_dbscan_t* m, const double* X,
    size_t rows, size_t cols, int* labels)
{
    size_t *neigh = NULL, *queue = NULL, *seed = NULL;
    unsigned char* queued = NULL;
    dyn_dbscan_grid_t grid;
    size_t i, qn, qi, nn, j, sn;
    int cluster = 0, have_grid;

    neigh = (size_t*)malloc(rows * sizeof(size_t));
    seed = (size_t*)malloc(rows * sizeof(size_t));
    queue = (size_t*)malloc(rows * sizeof(size_t));
    queued = (unsigned char*)calloc(rows, 1);
    if (!neigh || !seed || !queue || !queued) {
        free(neigh);
        free(seed);
        free(queue);
        free(queued);
        return MLK_CL_E_NOMEM;
    }
    have_grid = (cols <= DYN_DBSCAN_MAX_GRID_COLS && rows >= (size_t)1 << (cols * 2));
    memset(&grid, 0, sizeof(grid));
    if (have_grid && dyn_dbscan_grid_build(&grid, X, rows, cols, m->eps)) {
        free(neigh);
        free(seed);
        free(queue);
        free(queued);
        return MLK_CL_E_NOMEM;
    }
    for (i = 0; i < rows; i++)
        labels[i] = -1;

    for (i = 0; i < rows; i++) {
        if (labels[i] != -1 || queued[i])
            continue;
        nn = have_grid
            ? dyn_dbscan_grid_region(&grid, X, i, m->eps2, neigh)
            : dyn_dbscan_region(X, rows, cols, i, m->eps2, neigh);
        if (nn < m->min_pts)
            continue;
        labels[i] = cluster;
        qn = 0;
        for (j = 0; j < nn; j++) {
            size_t q = neigh[j];
            if (q == i || queued[q])
                continue;
            queued[q] = 1;
            queue[qn++] = q;
        }
        for (qi = 0; qi < qn; qi++) {
            size_t q = queue[qi];
            if (labels[q] == -1)
                labels[q] = cluster;
            sn = have_grid
                ? dyn_dbscan_grid_region(&grid, X, q, m->eps2, seed)
                : dyn_dbscan_region(X, rows, cols, q, m->eps2, seed);
            if (sn < m->min_pts)
                continue;
            for (j = 0; j < sn; j++) {
                size_t s = seed[j];
                if (queued[s] || labels[s] != -1)
                    continue;
                queued[s] = 1;
                queue[qn++] = s;
            }
        }
        cluster++;
    }
    free(grid.head);
    free(grid.next);
    free(grid.mgen);
    free(grid.mbkt);
    free(neigh);
    free(seed);
    free(queue);
    free(queued);
    return cluster;
}

double dyn_gmm_logpdf(const dyn_gmm_t* m, const double* x, size_t c)
{
    const double* mu = m->mean + c * m->cols;
    const double* iv = m->inv_var + c * m->cols;
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, d;
    size_t n = m->cols, j = 0;
#ifndef DYN_ML_NO_SIMD
    for (; j + 4 <= n; j += 4) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
        d = x[j + 1] - mu[j + 1];
        s1 += d * d * iv[j + 1];
        d = x[j + 2] - mu[j + 2];
        s2 += d * d * iv[j + 2];
        d = x[j + 3] - mu[j + 3];
        s3 += d * d * iv[j + 3];
    }
#endif
    for (; j < n; j++) {
        d = x[j] - mu[j];
        s0 += d * d * iv[j];
    }
    return m->lognorm[c] - 0.5 * ((s0 + s1) + (s2 + s3));
}

static void dyn_gmm_refresh(dyn_gmm_t* m)
{
    size_t c, j;
    for (c = 0; c < m->k; c++) {
        double ld = 0.0;
        for (j = 0; j < m->cols; j++) {
            double v = m->var[c * m->cols + j];
            if (!(v > 0.0))
                v = 1e-12;
            m->inv_var[c * m->cols + j] = 1.0 / v;
            ld += log(6.283185307179586476925286766559 * v);
        }
        m->lognorm[c] = log(m->weight[c]) - 0.5 * ld;
    }
}

double dyn_gmm_estep(const dyn_gmm_t* m, const double* X, size_t rows,
    double* resp)
{
    double total = 0.0;
    size_t i, c;
    for (i = 0; i < rows; i++) {
        const double* xi = X + i * m->cols;
        double best = -HUGE_VAL, sum = 0.0;
        for (c = 0; c < m->k; c++) {
            resp[i * m->k + c] = dyn_gmm_logpdf(m, xi, c);
            if (resp[i * m->k + c] > best)
                best = resp[i * m->k + c];
        }
        for (c = 0; c < m->k; c++) {
            resp[i * m->k + c] = exp(resp[i * m->k + c] - best);
            sum += resp[i * m->k + c];
        }
        for (c = 0; c < m->k; c++)
            resp[i * m->k + c] /= sum;
        total += best + log(sum);
    }
    return total;
}

int mlk_gmm_learn(dyn_gmm_t* m, const double* X,
    size_t rows, size_t cols, int (*poll)(void*), void* ud)
{
    double *weight = NULL, *mean = NULL, *var = NULL, *resp = NULL;
    double* nk_all = NULL;
    double *inv_var = NULL, *lognorm = NULL, *nearest = NULL;
    int* labels = NULL;
    size_t k = m->k, i, c, j, it;
    uint64_t rng = m->seed;
    double prev = -HUGE_VAL, ll = -HUGE_VAL;
    int rc = MLK_CL_E_NOMEM;

    if (rows < k)
        return MLK_CL_E_FEW_ROWS;
    if (rows > (SIZE_MAX / sizeof(double)) / k)
        return MLK_CL_E_TOO_BIG;
    weight = (double*)malloc(k * sizeof(double));
    mean = (double*)malloc(k * cols * sizeof(double));
    var = (double*)malloc(k * cols * sizeof(double));
    inv_var = (double*)malloc(k * cols * sizeof(double));
    lognorm = (double*)malloc(k * sizeof(double));
    resp = (double*)malloc(rows * k * sizeof(double));
    nk_all = (double*)malloc((k ? k : 1) * sizeof(double));
    nearest = (double*)malloc(rows * sizeof(double));
    labels = (int*)malloc(rows * sizeof(int));
    if (!weight || !mean || !var || !inv_var || !lognorm || !resp || !nearest || !labels || !nk_all)
        goto done;
    dyn_km_plusplus(X, rows, cols, k, mean, nearest, &rng);
    dyn_km_assign(X, rows, cols, mean, k, labels, NULL);
    m->cols = cols;
    m->weight = weight;
    m->mean = mean;
    m->var = var;
    m->inv_var = inv_var;
    m->lognorm = lognorm;
    for (c = 0; c < k; c++) {
        weight[c] = 0.0;
        for (j = 0; j < cols; j++)
            var[c * cols + j] = 0.0;
    }
    for (i = 0; i < rows; i++) {
        size_t lb = (size_t)labels[i];
        weight[lb] += 1.0;
        for (j = 0; j < cols; j++) {
            double d = X[i * cols + j] - mean[lb * cols + j];
            var[lb * cols + j] += d * d;
        }
    }
    for (c = 0; c < k; c++) {
        double cnt = weight[c];
        for (j = 0; j < cols; j++)
            var[c * cols + j] = (cnt > 0.0 ? var[c * cols + j] / cnt : 1.0) + m->reg;
        weight[c] = (cnt > 0.0 ? cnt : 1.0) / (double)rows;
    }
    dyn_gmm_refresh(m);
    for (it = 0; it < m->max_iter; it++) {
        if (poll(ud)) {
            rc = MLK_CL_E_INTR;
            goto done;
        }
        ll = dyn_gmm_estep(m, X, rows, resp);
        for (c = 0; c < k; c++)
            nk_all[c] = 0.0;
        for (i = 0; i < rows; i++)
            for (c = 0; c < k; c++)
                nk_all[c] += resp[i * k + c];

        for (c = 0; c < k; c++)
            for (j = 0; j < cols; j++)
                mean[c * cols + j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double* xi = X + i * cols;
            for (c = 0; c < k; c++) {
                double r = resp[i * k + c];
                if (nk_all[c] > 1e-300)
                    dyn_ml_axpy(mean + c * cols, r, xi, cols);
            }
        }
        for (c = 0; c < k; c++) {
            if (nk_all[c] <= 1e-300)
                continue;
            for (j = 0; j < cols; j++)
                mean[c * cols + j] /= nk_all[c];
        }

        for (c = 0; c < k; c++)
            for (j = 0; j < cols; j++)
                var[c * cols + j] = 0.0;
        for (i = 0; i < rows; i++) {
            const double* xi = X + i * cols;
            for (c = 0; c < k; c++) {
                double r = resp[i * k + c];
                if (nk_all[c] <= 1e-300)
                    continue;
                for (j = 0; j < cols; j++) {
                    double d = xi[j] - mean[c * cols + j];
                    var[c * cols + j] += r * d * d;
                }
            }
        }
        for (c = 0; c < k; c++) {
            if (nk_all[c] <= 1e-300) {
                weight[c] = 1e-300;
                continue;
            }
            for (j = 0; j < cols; j++)
                var[c * cols + j] = var[c * cols + j] / nk_all[c] + m->reg;
            weight[c] = nk_all[c] / (double)rows;
        }
        dyn_gmm_refresh(m);
        m->n_iter = it + 1;
        if (it > 0 && fabs(ll - prev) < m->tol * fabs(ll ? ll : 1.0))
            break;
        prev = ll;
    }
    m->loglik = dyn_gmm_estep(m, X, rows, resp);
    m->fitted = 1;
    weight = NULL;
    mean = NULL;
    var = NULL;
    inv_var = NULL;
    lognorm = NULL;
    rc = MLK_CL_OK;
done:
    if (rc != 0) {
        free(weight);
        free(mean);
        free(var);
        free(inv_var);
        free(lognorm);
        m->weight = NULL;
        m->mean = NULL;
        m->var = NULL;
        m->inv_var = NULL;
        m->lognorm = NULL;
    }
    free(resp);
    free(nearest);
    free(labels);
    free(nk_all);
    return rc;
}

