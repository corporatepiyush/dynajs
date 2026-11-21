/*
 * test_mlk_core.c -- src/core/mlk (the dyna:ml numeric kernels: linear
 * solve, Jacobi eigen-decomposition, the introsort family, CSR row kernels,
 * decision-tree build/predict) as a standalone C library: no engine.
 *
 * Expected values come from outside this code:
 *  - dyn_solve: an integer system built from a chosen solution
 *    (A x = b with x = (1, 2, 3)), so the answer is exact by construction.
 *  - dyn_jacobi: the n x n tridiagonal matrix tridiag(-1, 2, -1) has the
 *    closed-form eigenvalues 2 - 2 cos(k pi / (n + 1)), k = 1..n (the
 *    standard second-difference result).
 *  - dyn_ml_isort / dyn_ml_kv_sort_r against the C library's qsort(3) on
 *    random, sorted, reversed, constant and organ-pipe inputs at lengths that
 *    cross the insertion-sort and heapsort thresholds. kv_sort_r orders by key
 *    only (ties are unordered by contract), so its result is checked as
 *    "keys ascending" plus "the same (key, row) multiset"; one shape runs with
 *    a depth limit of 0 so the heapsort fallback sorts the whole array.
 *  - CSR dot/axpy against the dense loop.
 *  - Trees: on y = [x0 > 0.5] the tree must separate the training set
 *    exactly; on a step function y = (x0 < 0.3 ? 1 : 5) a depth-1 regressor
 *    must predict exactly 1 and 5 (the per-side means).
 *  - The interrupt contract: a poll that answers "stop" makes dyn_tree_fit
 *    return -1 with *interrupted set; no poll (NULL) never interrupts.
 *  - Persistence (the model codecs): the table holds the 20 published type
 *    ids 100..119 once each; encode -> decode -> encode is byte-identical and
 *    keeps every field; a record decoded by the wrong codec names the family
 *    it holds; every truncation and every single-byte flip of the header is
 *    refused or decoded without a fault (ASan); a classifier forest written
 *    under a regressor or boosting type id is refused as malformed (the
 *    family check), while the same forest under DTC and RFC loads.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-mlk.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static void test_solve(void)
{
    double A[9] = { 4, -2, 1, -2, 4, -2, 1, -2, 4 };
    double b[3] = { 3, 0, 9 };
    CHECK(dyn_solve(A, b, 3, 1e-12) == 0, "solve succeeds");
    CHECK(fabs(b[0] - 1) < 1e-12 && fabs(b[1] - 2) < 1e-12 && fabs(b[2] - 3) < 1e-12,
        "A x = b recovers x = (1, 2, 3): %.17g %.17g %.17g", b[0], b[1], b[2]);
    {
        double S[4] = { 1, 2, 2, 4 }, c[2] = { 1, 2 };
        CHECK(dyn_solve(S, c, 2, 1e-12) != 0, "a singular system is refused");
    }
    {
        double N1[4] = { 1, 1, 1, 1 + 1e-14 }, d1[2] = { 2, 2 + 1e-14 };
        double N2[4] = { 1, 1, 1, 1 + 1e-14 }, d2[2] = { 2, 2 + 1e-14 };
        CHECK(dyn_solve(N1, d1, 2, 1e-12) != 0, "a pivot below min_pivot is refused");
        CHECK(dyn_solve(N2, d2, 2, 1e-16) == 0, "the same system passes a smaller min_pivot");
    }
}

static int dcmp(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void test_jacobi(void)
{
    enum { N = 6 };
    double A[N * N], V[N * N], ev[N], want[N];
    size_t i, k;
    int ok = 1;
    memset(A, 0, sizeof A);
    for (i = 0; i < N; i++) {
        A[i * N + i] = 2;
        if (i + 1 < N)
            A[i * N + i + 1] = A[(i + 1) * N + i] = -1;
    }
    dyn_jacobi(A, V, N);
    for (i = 0; i < N; i++)
        ev[i] = A[i * N + i];
    qsort(ev, N, sizeof ev[0], dcmp);
    for (k = 1; k <= N; k++)
        want[k - 1] = 2 - 2 * cos((double)k * 3.14159265358979323846 / (N + 1));
    qsort(want, N, sizeof want[0], dcmp);
    for (i = 0; i < N; i++)
        if (fabs(ev[i] - want[i]) > 1e-12)
            ok = 0;
    CHECK(ok, "tridiag(-1,2,-1) eigenvalues match 2 - 2cos(k pi/(n+1))");
}

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int icmp(const void* a, const void* b)
{
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static void fill(int* v, size_t n, int shape)
{
    size_t i;
    for (i = 0; i < n; i++)
        v[i] = shape == 0 ? (int)(rnd() % 1000) : shape == 1 ? (int)i
            : shape == 2                                     ? (int)(n - i)
            : shape == 3                                     ? 7
                                                             : (int)(i < n / 2 ? i : n - i);
}

static void test_sorts(void)
{
    static const size_t lens[] = { 0, 1, 2, 3, 7, 15, 16, 17, 31, 64, 200, 1000, 4097 };
    size_t li, i;
    int shape, bad = 0;
    for (li = 0; li < sizeof lens / sizeof lens[0]; li++)
        for (shape = 0; shape < 5; shape++) {
            size_t n = lens[li];
            int* a = (int*)malloc((n + 1) * sizeof(int));
            int* b = (int*)malloc((n + 1) * sizeof(int));
            dyn_tree_kv_t* kv = (dyn_tree_kv_t*)malloc((n + 1) * sizeof(dyn_tree_kv_t));
            dyn_tree_kv_t* kr = (dyn_tree_kv_t*)malloc((n + 1) * sizeof(dyn_tree_kv_t));
            fill(a, n, shape);
            memcpy(b, a, n * sizeof(int));
            dyn_ml_isort(a, n, sizeof(int), icmp);
            qsort(b, n, sizeof(int), icmp);
            if (n && memcmp(a, b, n * sizeof(int)))
                bad++;
            for (i = 0; i < n; i++) {
                kv[i].key = (double)(rnd() % 50);
                kv[i].row = (uint32_t)i;
                kv[i].pad = 0;
            }
            memcpy(kr, kv, n * sizeof(*kv));
            dyn_ml_kv_sort_r(kv, n, 0, shape == 4 ? 0 : 64);
            qsort(kr, n, sizeof(*kr), dyn_ml_kv_cmp);
            for (i = 0; i < n; i++)
                if (kv[i].key != kr[i].key)
                    bad++;
            qsort(kv, n, sizeof(*kv), dyn_ml_kv_cmp);
            for (i = 0; i < n; i++)
                if (kv[i].row != kr[i].row)
                    bad++;
            free(a);
            free(b);
            free(kv);
            free(kr);
        }
    CHECK(bad == 0, "introsort family agrees with qsort(3) on every shape and length (%d bad)", bad);
}

static void test_csr(void)
{
    double val[4] = { 1.5, -2, 3, 0.25 }, dense[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, d2[8];
    uint32_t col[4] = { 0, 3, 5, 7 };
    double ref = 1.5 * 1 + -2 * 4 + 3 * 6 + 0.25 * 8;
    size_t i;
    int ok = 1;
    CHECK(dyn_csr_dot(val, col, 4, dense) == ref, "csr dot = dense dot (%.17g vs %.17g)", dyn_csr_dot(val, col, 4, dense), ref);
    memcpy(d2, dense, sizeof d2);
    dyn_csr_axpy(d2, 2.0, val, col, 4);
    for (i = 0; i < 8; i++) {
        double w = dense[i];
        size_t k;
        for (k = 0; k < 4; k++)
            if (col[k] == i)
                w += 2.0 * val[k];
        if (d2[i] != w)
            ok = 0;
    }
    CHECK(ok, "csr axpy = dense axpy");
}

static int polls, stop_at;
static int poll(void* ud)
{
    (void)ud;
    return ++polls >= stop_at;
}

typedef struct {
    uint32_t *idx, *scratch, *feat, *best_order;
    double cnt_l[2], cnt_r[2], cnt_tot[2];
    dyn_tree_kv_t* kv;
    uint8_t* cls_of;
    dyn_tree_work_t w;
} work_t;

static void work_init(work_t* k, size_t rows, size_t cols, const double* y, int classifier)
{
    size_t i;
    memset(k, 0, sizeof *k);
    k->idx = (uint32_t*)malloc(rows * sizeof(uint32_t));
    k->scratch = (uint32_t*)malloc(rows * sizeof(uint32_t));
    k->best_order = (uint32_t*)malloc(rows * sizeof(uint32_t));
    k->feat = (uint32_t*)malloc(cols * sizeof(uint32_t));
    k->kv = (dyn_tree_kv_t*)malloc(rows * sizeof(dyn_tree_kv_t));
    k->cls_of = (uint8_t*)malloc(rows);
    for (i = 0; i < rows; i++) {
        k->idx[i] = (uint32_t)i;
        k->cls_of[i] = (uint8_t)(y[i] != 0);
    }
    k->w.scratch = k->scratch;
    k->w.feat = k->feat;
    k->w.cnt_l = k->cnt_l;
    k->w.cnt_r = k->cnt_r;
    k->w.cnt_tot = k->cnt_tot;
    k->w.kv = k->kv;
    k->w.best_order = k->best_order;
    k->w.cls_of = classifier ? k->cls_of : NULL;
}

static void work_free(work_t* k)
{
    free(k->idx);
    free(k->scratch);
    free(k->best_order);
    free(k->feat);
    free(k->kv);
    free(k->cls_of);
}

static void test_tree(void)
{
    enum { R = 200,
        C = 2 };
    double X[R * C], y[R], ys[R];
    static const double classes[2] = { 0, 1 };
    dyn_tree_opts_t o;
    dyn_tree_t t;
    work_t k;
    uint64_t rng = 42;
    size_t i;
    int itr = 0, ok = 1;
    for (i = 0; i < R; i++) {
        X[i * C] = (double)i / R;
        X[i * C + 1] = (double)((i * 37) % R) / R;
        y[i] = X[i * C] > 0.5 ? 1 : 0;
        ys[i] = X[i * C] < 0.3 ? 1 : 5;
    }
    memset(&o, 0, sizeof o);
    o.max_depth = 8;
    o.min_samples_split = 2;
    o.min_samples_leaf = 1;
    o.classifier = 1;
    memset(&t, 0, sizeof t);
    work_init(&k, R, C, y, 1);
    CHECK(dyn_tree_fit(&t, X, y, R, C, classes, 2, &o, &rng, k.idx, R, &k.w, NULL, NULL, &itr) == 0, "classifier fits");
    for (i = 0; i < R; i++)
        if (dyn_tree_predict_row(&t, X + i * C) != y[i])
            ok = 0;
    CHECK(ok && t.n_nodes >= 3, "the tree separates y = [x0 > 0.5] exactly (%zu nodes)", t.n_nodes);
    dyn_tree_free(&t);
    work_free(&k);

    o.classifier = 0;
    o.max_depth = 1;
    memset(&t, 0, sizeof t);
    work_init(&k, R, C, ys, 0);
    CHECK(dyn_tree_fit(&t, X, ys, R, C, NULL, 1, &o, &rng, k.idx, R, &k.w, NULL, NULL, &itr) == 0, "regressor fits");
    CHECK(dyn_tree_predict_row(&t, X) == 1 && dyn_tree_predict_row(&t, X + (R - 1) * C) == 5,
        "a depth-1 regressor predicts the per-side means 1 and 5 exactly");
    dyn_tree_free(&t);
    work_free(&k);

    o.classifier = 1;
    o.max_depth = 8;
    memset(&t, 0, sizeof t);
    work_init(&k, R, C, y, 1);
    polls = 0;
    stop_at = 3;
    itr = 0;
    CHECK(dyn_tree_fit(&t, X, y, R, C, classes, 2, &o, &rng, k.idx, R, &k.w, poll, NULL, &itr) == -1 && itr == 1,
        "a poll that says stop interrupts the build (rc -1, interrupted %d, %d polls)", itr, polls);
    CHECK(polls == 3, "the build polls once per node and stops at the first 'stop' (%d)", polls);
    dyn_tree_free(&t);
    memset(&t, 0, sizeof t);
    polls = 0;
    stop_at = 1 << 30;
    itr = 0;
    CHECK(dyn_tree_fit(&t, X, y, R, C, classes, 2, &o, &rng, k.idx, R, &k.w, poll, NULL, &itr) == 0 && itr == 0
            && (size_t)polls == t.n_nodes,
        "a poll that never stops is asked once per node (%d polls, %zu nodes)", polls, t.n_nodes);
    dyn_tree_free(&t);
    work_free(&k);
}

static void test_persist_table(void)
{
    int seen[20], i, ok = 1;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < DYN_MLK_N_CODECS; i++) {
        const dyn_mlk_codec_t* c = &dyn_mlk_codecs[i];
        if (c->type_id < 100 || c->type_id > 119 || seen[c->type_id - 100]++ || !c->name || !c->write || !c->read
            || !c->dispose || !c->is_fitted)
            ok = 0;
    }
    CHECK(DYN_MLK_N_CODECS == 20 && ok, "20 codecs, type ids 100..119 each once, every hook set");
    CHECK(dyn_mlk_codecs[DYN_MLK_C_LINREG].type_id == DYN_TID_ML_LINREG
            && dyn_mlk_codecs[DYN_MLK_C_XGBC].type_id == DYN_TID_ML_XGBC
            && !strcmp(dyn_mlk_codecs[DYN_MLK_C_KNN_REG].name, "KNRegressor"),
        "index enum, type ids and names agree");
}

static void test_persist_linear(void)
{
    const dyn_mlk_codec_t* L = &dyn_mlk_codecs[DYN_MLK_C_LINREG];
    dyn_linreg_t* m = (dyn_linreg_t*)calloc(1, sizeof(*m));
    dyn_linreg_t* back;
    uint8_t *a, *b;
    size_t na = 0, nb = 0, cut;
    void* out;
    int rc, de, bad = 0;
    const char* held;
    m->fitted = 1;
    m->n_features = 3;
    m->intercept = -0.25;
    m->coef = (double*)malloc(3 * sizeof(double));
    m->coef[0] = 1.5;
    m->coef[1] = -2;
    m->coef[2] = 1e-300;
    CHECK(L->is_fitted(m), "is_fitted reads the flag");
    a = dyn_mlk_encode(L, m, &na);
    rc = dyn_mlk_decode(L, a, na, &out, &de, &held);
    back = (dyn_linreg_t*)out;
    CHECK(rc == DYN_MLK_DEC_OK && back && back->fitted && back->n_features == 3 && back->intercept == -0.25
            && !memcmp(back->coef, m->coef, 3 * sizeof(double)),
        "linear round trip keeps every field (rc %d)", rc);
    b = back ? dyn_mlk_encode(L, back, &nb) : NULL;
    CHECK(b && na == nb && !memcmp(a, b, na), "re-encoding is byte-identical");
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_KMEANS], a, na, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_WRONG_TYPE && held && !strcmp(held, "LinearRegression") && !out,
        "the wrong codec names the family the record holds");
    for (cut = 0; cut < na; cut++) {
        rc = dyn_mlk_decode(L, a, cut, &out, &de, &held);
        if (rc == DYN_MLK_DEC_OK || out)
            bad++;
        if (rc == DYN_MLK_DEC_FORMAT && de == DYN_DE_OK)
            bad++;
    }
    CHECK(bad == 0, "every truncation of a %zu-byte record is refused (%d accepted)", na, bad);
    {
        uint8_t* t = (uint8_t*)malloc(na + 1);
        memcpy(t, a, na);
        t[na] = 0;
        rc = dyn_mlk_decode(L, t, na + 1, &out, &de, &held);
        CHECK(rc != DYN_MLK_DEC_OK && !out, "a trailing byte is refused (rc %d)", rc);
        for (cut = 0; cut < na; cut++) {
            memcpy(t, a, na);
            t[cut] ^= 0x41;
            rc = dyn_mlk_decode(L, t, na, &out, &de, &held);
            if (rc == DYN_MLK_DEC_OK)
                L->dispose(out);
        }
        free(t);
    }
    L->dispose(back);
    L->dispose(m);
    free(a);
    free(b);
}

static void test_persist_forest(void)
{
    enum { R = 120,
        C = 2 };
    double X[R * C], y[R];
    static const double classes[2] = { 0, 1 };
    dyn_tree_opts_t o;
    work_t k;
    uint64_t rng = 7;
    int itr = 0, ok = 1;
    size_t i, n = 0;
    dyn_forest_t* f = (dyn_forest_t*)calloc(1, sizeof(*f));
    uint8_t* blob;
    void* out;
    int rc, de;
    const char* held;
    for (i = 0; i < R; i++) {
        X[i * C] = (double)i / R;
        X[i * C + 1] = (double)((i * 53) % R) / R;
        y[i] = X[i * C] > 0.4 ? 1 : 0;
    }
    memset(&o, 0, sizeof o);
    o.max_depth = 6;
    o.min_samples_split = 2;
    o.min_samples_leaf = 1;
    o.classifier = 1;
    f->trees = (dyn_tree_t*)calloc(1, sizeof(dyn_tree_t));
    work_init(&k, R, C, y, 1);
    CHECK(dyn_tree_fit(&f->trees[0], X, y, R, C, classes, 2, &o, &rng, k.idx, R, &k.w, NULL, NULL, &itr) == 0,
        "forest tree fits");
    work_free(&k);
    f->fitted = 1;
    f->classifier = 1;
    f->n_trees = 1;
    f->n_rounds = 1;
    f->n_features = C;
    f->n_classes = 2;
    f->classes = (double*)malloc(sizeof classes);
    memcpy(f->classes, classes, sizeof classes);
    f->subsample = 1;
    f->colsample = 1;
    f->opt = o;
    blob = dyn_mlk_encode(&dyn_mlk_codecs[DYN_MLK_C_DTC], f, &n);
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_DTC], blob, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_OK, "a classifier tree loads as DecisionTreeClassifier (rc %d)", rc);
    if (rc == DYN_MLK_DEC_OK) {
        const dyn_forest_t* g = (const dyn_forest_t*)out;
        for (i = 0; i < R; i++)
            if (dyn_tree_predict_row(&g->trees[0], X + i * C) != dyn_tree_predict_row(&f->trees[0], X + i * C))
                ok = 0;
        CHECK(ok && g->trees[0].n_nodes == f->trees[0].n_nodes, "the loaded tree predicts identically");
        dyn_forest_dispose(out);
    }
    free(blob);
    blob = dyn_mlk_encode(&dyn_mlk_codecs[DYN_MLK_C_RFC], f, &n);
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_RFC], blob, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_OK, "the same forest loads as RandomForestClassifier");
    if (rc == DYN_MLK_DEC_OK)
        dyn_forest_dispose(out);
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_DTC], blob, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_WRONG_TYPE && held && !strcmp(held, "RandomForestClassifier"), "RFC record is not a DTC");
    free(blob);
    blob = dyn_mlk_encode(&dyn_mlk_codecs[DYN_MLK_C_DTR], f, &n);
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_DTR], blob, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_MALFORMED && !out, "a classifier under the regressor type id is malformed (rc %d)", rc);
    free(blob);
    blob = dyn_mlk_encode(&dyn_mlk_codecs[DYN_MLK_C_GBC], f, &n);
    rc = dyn_mlk_decode(&dyn_mlk_codecs[DYN_MLK_C_GBC], blob, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_MALFORMED && !out, "a non-boosted forest under the GBC type id is malformed (rc %d)", rc);
    free(blob);
    CHECK(dyn_dbscan_eps_ok(0.5) && !dyn_dbscan_eps_ok(0) && !dyn_dbscan_eps_ok(1e200) && !dyn_dbscan_eps_ok(-1),
        "DBSCAN eps must be positive with a finite square");
    dyn_forest_dispose(f);
}

static uint8_t* framed(uint16_t tid, int fields, int extra, size_t* n)
{
    dyn_ser_t w;
    dyn_ser_init(&w);
    dyn_ser_begin(&w, tid, 0);
    dyn_ser_u64(&w, 1);
    if (fields) {
        dyn_ser_f64(&w, 0.5);
        dyn_ser_f64(&w, 2.0);
    }
    if (extra)
        dyn_ser_u32(&w, 0xdead);
    dyn_ser_finish(&w);
    return dyn_ser_take(&w, n);
}

static void test_persist_framed(void)
{
    const dyn_mlk_codec_t* L = &dyn_mlk_codecs[DYN_MLK_C_LINREG];
    size_t n;
    void* out;
    int rc, de;
    const char* held;
    uint8_t* b = framed(DYN_TID_ML_LINREG, 1, 0, &n);
    rc = dyn_mlk_decode(L, b, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_OK && ((dyn_linreg_t*)out)->coef[0] == 2.0, "a hand-framed linear record loads");
    if (rc == DYN_MLK_DEC_OK)
        L->dispose(out);
    free(b);
    b = framed(DYN_TID_ML_LINREG, 0, 0, &n);
    rc = dyn_mlk_decode(L, b, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_MALFORMED && !out, "a well-framed record missing fields is malformed (rc %d)", rc);
    free(b);
    b = framed(DYN_TID_ML_LINREG, 1, 1, &n);
    rc = dyn_mlk_decode(L, b, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_MALFORMED && !out, "unread payload bytes make a record malformed (rc %d)", rc);
    free(b);
    b = framed(150, 1, 0, &n);
    rc = dyn_mlk_decode(L, b, n, &out, &de, &held);
    CHECK(rc == DYN_MLK_DEC_WRONG_TYPE && !held, "an unknown type id holds no known family");
    free(b);
}

int main(void)
{
    test_solve();
    test_jacobi();
    test_sorts();
    test_csr();
    test_tree();
    test_persist_table();
    test_persist_linear();
    test_persist_forest();
    test_persist_framed();
    printf("test_mlk_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
