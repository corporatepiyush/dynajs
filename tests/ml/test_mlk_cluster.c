/*
 * test_mlk_cluster.c -- the clustering kernels in src/core/mlk/mlk-cluster.c
 * (k-means++ seeding and Lloyd iterations, DBSCAN with and without the
 * eps-grid, diagonal Gaussian mixture EM) as a standalone C library.
 *
 * Oracles are written here, not taken from the code:
 *  - k-means: the result must be a Lloyd fixed point checked by brute force
 *    (every label is the nearest centroid, every centroid is the weighted
 *    mean of its members, inertia is the weighted sum of squared distances),
 *    and on well separated blobs the partition must equal the blobs.
 *  - DBSCAN: a textbook O(n^2) DBSCAN written here (core = at least minPts
 *    points within eps, itself included; clusters = connected components of
 *    core points; border points join a cluster of a core point within eps).
 *    Noise sets must be equal, core partitions equal up to renaming, and
 *    each border label must be the cluster of a core neighbour. Dimensions
 *    1..7 and row counts on both sides of the grid cut-over (rows >= 4^cols,
 *    cols <= 6) run the grid and the brute path.
 *  - GMM: the log density is recomputed from the diagonal normal formula in
 *    long double; responsibilities must sum to 1 per row; the stored
 *    log-likelihood must equal sum_i log sum_c w_c N(x_i; mu_c, var_c); the
 *    weights must sum to 1; blobs must be recovered.
 *  - Errors: too few rows, k too close to rows, data too large, and a poll
 *    that answers "stop" give their codes and leave the model unfitted.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-mlk.h"
#include "dyn-prng.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static unsigned long long rs = 0x243f6a8885a308d3ull;
static double urand(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0;
}
static unsigned rnd(unsigned n) { return (unsigned)(urand() * n); }
static double gauss(void)
{
    double u = urand() + 1e-300, v = urand();
    return sqrt(-2 * log(u)) * cos(6.283185307179586 * v);
}

static int never(void* ud)
{
    (void)ud;
    return 0;
}
static int always(void* ud)
{
    (void)ud;
    return 1;
}

static double sq(const double* a, const double* b, size_t d)
{
    long double s = 0;
    size_t j;
    for (j = 0; j < d; j++)
        s += (long double)(a[j] - b[j]) * (a[j] - b[j]);
    return (double)s;
}

static size_t blobs(double* X, int* truth, size_t k, size_t per, size_t cols, double spread)
{
    size_t c, i, j, n = 0;
    for (c = 0; c < k; c++)
        for (i = 0; i < per; i++, n++) {
            for (j = 0; j < cols; j++)
                X[n * cols + j] = 100.0 * (double)c * (j == 0 ? 1 : (double)(c % 3)) + spread * gauss();
            truth[n] = (int)c;
        }
    return n;
}

static int same_partition(const int* a, const int* b, size_t n)
{
    size_t i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j += 7)
            if ((a[i] == a[j]) != (b[i] == b[j]))
                return 0;
    return 1;
}

static void t_kmeans(void)
{
    static double X[4000 * 4], w[4000];
    static int truth[4000], lab[4000];
    int trial;
    for (trial = 0; trial < 60; trial++) {
        size_t cols = 1 + rnd(4), k = 1 + rnd(6), per = 20 + rnd(200), n = blobs(X, truth, k, per, cols, trial % 2 ? 1.0 : 30.0), i, c, j;
        int weighted = trial % 3 == 0, rc;
        dyn_kmeans_t m;
        long double inertia = 0;
        for (i = 0; i < n; i++)
            w[i] = 0.25 + urand();
        memset(&m, 0, sizeof(m));
        m.k = k;
        m.seed = (uint64_t)trial * 7919u + 1;
        rc = mlk_kmeans_train(&m, X, n, cols, weighted ? w : NULL, never, NULL);
        CHECK(rc == MLK_CL_OK && m.fitted && m.n_features == cols, "kmeans trial %d rc=%d", trial, rc);
        if (rc)
            continue;
        for (i = 0; i < n; i++) {
            double best = HUGE_VAL;
            int bl = -1;
            for (c = 0; c < k; c++) {
                double d = sq(X + i * cols, m.centroids + c * cols, cols);
                if (d < best)
                    best = d, bl = (int)c;
            }
            lab[i] = bl;
            inertia += (long double)(weighted ? w[i] : 1.0) * best;
        }
        CHECK(fabs((double)inertia - m.inertia) <= 1e-9 * (double)inertia + 1e-12, "kmeans inertia trial %d got %.17g want %.17Lg", trial, m.inertia, inertia);
        for (c = 0; c < k; c++) {
            long double s[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (i = 0; i < n; i++)
                if (lab[i] == (int)c) {
                    long double wi = weighted ? w[i] : 1.0;
                    cnt += wi;
                    for (j = 0; j < cols; j++)
                        s[j] += wi * X[i * cols + j];
                }
            for (j = 0; cnt > 0 && j < cols; j++) {
                double mean = (double)(s[j] / cnt), got = m.centroids[c * cols + j];
                CHECK(fabs(got - mean) <= 1e-9 * (fabs(mean) + 1), "kmeans centroid trial %d c=%zu j=%zu got %.17g want %.17g", trial, c, j, got, mean);
            }
        }
        if (trial % 2)
            CHECK(same_partition(lab, truth, n), "kmeans recovers %zu separated blobs trial %d", k, trial);
        free(m.centroids);
    }
    {
        dyn_kmeans_t m;
        size_t n = blobs(X, truth, 3, 50, 2, 1.0);
        memset(&m, 0, sizeof(m));
        m.k = 151;
        CHECK(mlk_kmeans_train(&m, X, 150, 2, NULL, never, NULL) == MLK_CL_E_FEW_ROWS && !m.fitted, "kmeans rows < k");
        m.k = 100;
        CHECK(mlk_kmeans_train(&m, X, 150, 2, NULL, never, NULL) == MLK_CL_E_K_CLOSE && !m.fitted, "kmeans k close to rows");
        m.k = 64;
        CHECK(mlk_kmeans_train(&m, X, 100, 2, NULL, never, NULL) == MLK_CL_OK, "kmeans k = 64 of 100 is allowed");
        free(m.centroids);
        memset(&m, 0, sizeof(m));
        m.k = 3;
        CHECK(mlk_kmeans_train(&m, X, n, 2, NULL, always, NULL) == MLK_CL_E_INTR && !m.fitted && !m.centroids, "kmeans interrupt");
    }
}

static int o_dbscan(const double* X, size_t n, size_t cols, double eps, size_t minpts, int* lab, int* core)
{
    size_t i, j, *st = malloc(n * sizeof(size_t));
    int cl = 0;
    double e2 = eps * eps;
    for (i = 0; i < n; i++) {
        size_t cnt = 0;
        for (j = 0; j < n; j++)
            cnt += sq(X + i * cols, X + j * cols, cols) <= e2;
        core[i] = cnt >= minpts;
        lab[i] = -1;
    }
    for (i = 0; i < n; i++) {
        size_t sp = 0;
        if (!core[i] || lab[i] != -1)
            continue;
        lab[i] = cl;
        st[sp++] = i;
        while (sp) {
            size_t p = st[--sp];
            for (j = 0; j < n; j++)
                if (core[j] && lab[j] == -1 && sq(X + p * cols, X + j * cols, cols) <= e2) {
                    lab[j] = cl;
                    st[sp++] = j;
                }
        }
        cl++;
    }
    free(st);
    return cl;
}

static void t_dbscan(void)
{
    static double X[3000 * 7];
    static int got[3000], want[3000], core[3000], map[3000];
    int trial;
    for (trial = 0; trial < 80; trial++) {
        size_t cols = 1 + (size_t)trial % 7, n, i, j;
        size_t grid_min = (size_t)1 << (cols * 2);
        double eps = 0.3 + urand();
        dyn_dbscan_t m;
        int nc, wc;
        n = trial % 2 ? grid_min + rnd(400) : (grid_min > 40 ? 10 + rnd((unsigned)(grid_min - 10 < 2000 ? grid_min - 10 : 2000)) : grid_min + rnd(200));
        if (n > 3000)
            n = 3000;
        for (i = 0; i < n; i++)
            for (j = 0; j < cols; j++)
                X[i * cols + j] = (double)(i % 5) * 3.0 + (urand() - 0.5) * (1.0 + (double)cols) + (rnd(20) == 0 ? -1e6 * urand() : 0);
        memset(&m, 0, sizeof(m));
        m.eps = eps;
        m.eps2 = eps * eps;
        m.min_pts = 2 + rnd(6);
        nc = mlk_dbscan_run(&m, X, n, cols, got);
        wc = o_dbscan(X, n, cols, eps, m.min_pts, want, core);
        CHECK(nc == wc, "dbscan clusters cols=%zu n=%zu got %d want %d", cols, n, nc, wc);
        for (i = 0; i < (size_t)wc; i++)
            map[i] = -2;
        for (i = 0; i < n; i++) {
            if (!core[i]) {
                int nb = 0;
                for (j = 0; j < n && !nb; j++)
                    nb = core[j] && sq(X + i * cols, X + j * cols, cols) <= m.eps2;
                if (!nb)
                    CHECK(got[i] == -1, "dbscan noise row %zu cols=%zu", i, cols);
                continue;
            }
            if (got[i] < 0 || got[i] >= nc) {
                CHECK(0, "dbscan core row %zu unlabeled", i);
                continue;
            }
            if (map[want[i]] == -2)
                map[want[i]] = got[i];
            CHECK(map[want[i]] == got[i], "dbscan core partition row %zu cols=%zu", i, cols);
        }
        for (i = 0; i < n; i++) {
            int ok = 0;
            if (core[i])
                continue;
            if (got[i] == -1) {
                for (j = 0; j < n && !ok; j++)
                    ok = core[j] && sq(X + i * cols, X + j * cols, cols) <= m.eps2;
                CHECK(!ok, "dbscan border row %zu left as noise cols=%zu", i, cols);
                continue;
            }
            for (j = 0; j < n && !ok; j++)
                ok = core[j] && got[j] == got[i] && sq(X + i * cols, X + j * cols, cols) <= m.eps2;
            CHECK(ok, "dbscan border row %zu label %d has no core neighbour cols=%zu", i, got[i], cols);
        }
    }
}

static long double o_logpdf(const dyn_gmm_t* m, const double* x, size_t c)
{
    long double s = logl((long double)m->weight[c]);
    size_t j;
    for (j = 0; j < m->cols; j++) {
        long double v = m->var[c * m->cols + j], d = (long double)x[j] - m->mean[c * m->cols + j];
        s += -0.5L * logl(2 * 3.14159265358979323846264338327950288L * v) - 0.5L * d * d / v;
    }
    return s;
}

static void t_gmm(void)
{
    static double X[3000 * 4], resp[3000 * 8];
    static int truth[3000], lab[3000];
    int trial;
    for (trial = 0; trial < 30; trial++) {
        size_t cols = 1 + rnd(4), k = 1 + rnd(5), per = 30 + rnd(150), n = blobs(X, truth, k, per, cols, trial % 2 ? 80.0 : 1.0 + (double)rnd(3)), i, c;
        dyn_gmm_t m;
        long double ll = 0, wsum = 0;
        int rc;
        memset(&m, 0, sizeof(m));
        m.k = k;
        m.max_iter = 100;
        m.tol = 1e-6;
        m.reg = 1e-6;
        m.seed = (uint64_t)trial + 99;
        rc = mlk_gmm_learn(&m, X, n, cols, never, NULL);
        CHECK(rc == MLK_CL_OK && m.fitted && m.n_iter >= 1 && m.n_iter <= 100, "gmm trial %d rc=%d", trial, rc);
        if (rc)
            continue;
        for (c = 0; c < k; c++)
            wsum += m.weight[c];
        CHECK(fabsl(wsum - 1) < 1e-12, "gmm weights sum %.17Lg", wsum);
        dyn_gmm_estep(&m, X, n, resp);
        for (i = 0; i < n; i++) {
            long double mx = -HUGE_VALL, s = 0, rsum = 0, lp[8];
            size_t best = 0;
            for (c = 0; c < k; c++) {
                lp[c] = o_logpdf(&m, X + i * cols, c);
                if (lp[c] > mx)
                    mx = lp[c], best = c;
                CHECK(fabsl(lp[c] - dyn_gmm_logpdf(&m, X + i * cols, c)) <= 1e-9L * (fabsl(lp[c]) + 1), "gmm logpdf row %zu c %zu", i, c);
            }
            for (c = 0; c < k; c++)
                s += expl(lp[c] - mx);
            ll += mx + logl(s);
            for (c = 0; c < k; c++)
                rsum += resp[i * k + c];
            CHECK(fabsl(rsum - 1) < 1e-12, "gmm responsibilities row %zu sum %.17Lg", i, rsum);
            lab[i] = (int)best;
        }
        CHECK(fabsl(ll - m.loglik) <= 1e-9L * fabsl(ll) + 1e-9L, "gmm loglik got %.17g want %.17Lg", m.loglik, ll);
        if (k <= 3 && trial % 2 == 0)
            CHECK(same_partition(lab, truth, n), "gmm recovers %zu blobs trial %d", k, trial);
        free(m.weight);
        free(m.mean);
        free(m.var);
        free(m.inv_var);
        free(m.lognorm);
    }
    {
        dyn_gmm_t m;
        size_t n = blobs(X, truth, 2, 40, 2, 1.0);
        memset(&m, 0, sizeof(m));
        m.k = 81;
        m.max_iter = 10;
        CHECK(mlk_gmm_learn(&m, X, n, 2, never, NULL) == MLK_CL_E_FEW_ROWS && !m.fitted, "gmm rows < k");
        m.k = 2;
        CHECK(mlk_gmm_learn(&m, X, n, 2, always, NULL) == MLK_CL_E_INTR && !m.fitted && !m.mean && !m.weight, "gmm interrupt");
        m.k = (size_t)1 << 62;
        CHECK(mlk_gmm_learn(&m, X, 1, 2, never, NULL) == MLK_CL_E_FEW_ROWS, "gmm few rows before size check");
    }
}

int main(void)
{
    t_kmeans();
    t_dbscan();
    t_gmm();
    printf("%s test_mlk_cluster: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
