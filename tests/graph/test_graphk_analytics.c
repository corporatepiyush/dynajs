/*
 * test_graphk_analytics.c -- the CSR analytics kernels of src/core/graphk
 * (PageRank, label propagation, Louvain, modularity, k-cores, triangles,
 * clustering, betweenness, closeness, SCC, WCC, topological levels, Dinic
 * max-flow, Hopcroft-Karp, link prediction) against their DEFINITIONS run
 * by brute force on small seeded random graphs:
 *  - PageRank: a dense power iteration over the full transition matrix;
 *  - triangles: every vertex triple of the adjacency matrix;
 *  - core numbers: repeated deletion of every vertex of degree < k;
 *  - betweenness: sum over ordered pairs of sigma_st(v) / sigma_st from
 *    all-pairs BFS (unweighted) or Floyd-Warshall path counts (weighted);
 *  - closeness: all-pairs distances to each vertex;
 *  - SCC: u ~ v iff each reaches the other (boolean transitive closure);
 *  - max-flow: the minimum over all 2^(n-2) s-t cuts (max-flow/min-cut);
 *  - matching: the maximum matching by exhaustive subset search;
 *  - modularity: sum_ij (A_ij - k_i k_j / 2m) [c_i = c_j] / 2m.
 * Parallel kernels must give BIT-IDENTICAL results at 1 thread and at the
 * default thread count (DYNAJS_PAR_SPAWN_LIMIT pins the helper count).
 * Edge cases: empty graph, isolated vertices, self loops, parallel edges,
 * directed vs undirected, disconnected graphs.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-graphk.h"
#include "dyn-par.h"
#include "dyn-pool.h"
#include "../lib/dyn-xalloc.h"

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

#define MAXN 24

static uint64_t rs = 88172645463325252ull;

static uint32_t rnd(uint32_t n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs % n);
}

typedef struct {
    uint32_t n, m;
    uint32_t u[512], v[512];
    double w[512];
    int directed;
} el_t;

static void make(el_t* g, uint32_t n, uint32_t m, int directed, int weighted, int loops)
{
    uint32_t i;
    g->n = n;
    g->m = 0;
    g->directed = directed;
    for (i = 0; i < m && n > 0; i++) {
        uint32_t a = rnd(n), b = rnd(n);
        if (a == b && !loops)
            continue;
        g->u[g->m] = a;
        g->v[g->m] = b;
        g->w[g->m] = weighted ? 1.0 + rnd(9) + (double)rnd(4) / 4.0 : 1.0;
        g->m++;
    }
}

static void to_csr(const el_t* g, int with_w, dyn_gk_csr_t* c)
{
    uint32_t i, *pos;
    uint64_t arcs = 0;
    for (i = 0; i < g->m; i++)
        arcs += (!g->directed && g->u[i] != g->v[i]) ? 2 : 1;
    dyn_gk_csr_alloc(c, g->n, arcs, with_w);
    pos = (uint32_t*)calloc(g->n + 1, sizeof(uint32_t));
    for (i = 0; i < g->m; i++) {
        c->off[g->u[i] + 1]++;
        if (!g->directed && g->u[i] != g->v[i])
            c->off[g->v[i] + 1]++;
    }
    for (i = 0; i < g->n; i++)
        c->off[i + 1] += c->off[i];
    memcpy(pos, c->off, g->n * sizeof(uint32_t));
    for (i = 0; i < g->m; i++) {
        uint32_t p = pos[g->u[i]]++;
        c->tgt[p] = g->v[i];
        if (with_w)
            c->w[p] = g->w[i];
        if (!g->directed && g->u[i] != g->v[i]) {
            p = pos[g->v[i]]++;
            c->tgt[p] = g->u[i];
            if (with_w)
                c->w[p] = g->w[i];
        }
    }
    free(pos);
}

static void adj_simple(const el_t* g, int und, uint8_t a[MAXN][MAXN])
{
    uint32_t i;
    memset(a, 0, sizeof(uint8_t) * MAXN * MAXN);
    for (i = 0; i < g->m; i++) {
        if (g->u[i] == g->v[i])
            continue;
        a[g->u[i]][g->v[i]] = 1;
        if (und || !g->directed)
            a[g->v[i]][g->u[i]] = 1;
    }
}

static void test_csr_helpers(void)
{
    int t;
    for (t = 0; t < 200; t++) {
        el_t g;
        dyn_gk_csr_t c, s, d, tr;
        uint8_t a[MAXN][MAXN];
        uint32_t n = 1 + rnd(MAXN), u, e, x, y;
        int ok = 1;
        make(&g, n, rnd(60), rnd(2), 1, 1);
        to_csr(&g, 1, &c);
        CHECK(dyn_gk_csr_validate(&c) == DYN_GK_OK, "built CSR validates");
        dyn_gk_csr_sym(&c, 0, 0, &s);
        dyn_gk_csr_dedup(&c, 1, &d);
        dyn_gk_csr_transpose(&c, 1, &tr);
        adj_simple(&g, 1, a);
        for (x = 0; x < n; x++) {
            uint32_t cnt = 0;
            for (y = 0; y < n; y++)
                cnt += a[x][y];
            if (cnt != s.off[x + 1] - s.off[x])
                ok = 0;
            for (e = s.off[x]; e < s.off[x + 1]; e++) {
                if (!a[x][s.tgt[e]] || (e > s.off[x] && s.tgt[e - 1] >= s.tgt[e]))
                    ok = 0;
            }
        }
        CHECK(ok, "graph %d: sym is the sorted simple undirected adjacency", t);
        ok = 1;
        for (u = 0; u < n; u++)
            for (e = d.off[u]; e < d.off[u + 1]; e++) {
                double mn = INFINITY;
                uint32_t i;
                for (i = 0; i < g.m; i++) {
                    int hit = (g.u[i] == u && g.v[i] == d.tgt[e]) || (!g.directed && g.v[i] == u && g.u[i] == d.tgt[e]);
                    if (hit && g.w[i] < mn)
                        mn = g.w[i];
                }
                if (d.w[e] != mn || d.tgt[e] == u || (e > d.off[u] && d.tgt[e - 1] >= d.tgt[e]))
                    ok = 0;
            }
        CHECK(ok, "graph %d: dedup keeps the minimum parallel weight, sorted, no loops", t);
        CHECK(tr.m == c.m, "graph %d: transpose keeps every arc", t);
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&s);
        dyn_gk_csr_free(&d);
        dyn_gk_csr_free(&tr);
    }
}

static void dense_pagerank(const el_t* g, double alpha, double tol, double* x)
{
    uint32_t n = g->n, i, j, it;
    static double W[MAXN][MAXN];
    double out[MAXN], nx[MAXN];
    memset(W, 0, sizeof W);
    memset(out, 0, sizeof out);
    for (i = 0; i < g->m; i++) {
        W[g->u[i]][g->v[i]] += g->w[i];
        out[g->u[i]] += g->w[i];
        if (!g->directed && g->u[i] != g->v[i]) {
            W[g->v[i]][g->u[i]] += g->w[i];
            out[g->v[i]] += g->w[i];
        }
    }
    for (i = 0; i < n; i++)
        x[i] = 1.0 / n;
    for (it = 0; it < 10000; it++) {
        double dang = 0.0, err = 0.0;
        for (i = 0; i < n; i++)
            if (out[i] == 0.0)
                dang += x[i];
        for (j = 0; j < n; j++) {
            double s = 0.0;
            for (i = 0; i < n; i++)
                if (out[i] > 0.0)
                    s += x[i] * W[i][j] / out[i];
            nx[j] = alpha * (s + dang / n) + (1.0 - alpha) / n;
        }
        for (i = 0; i < n; i++) {
            err += fabs(nx[i] - x[i]);
            x[i] = nx[i];
        }
        if (err < n * tol)
            break;
    }
}

static void test_pagerank(void)
{
    int t;
    for (t = 0; t < 150; t++) {
        el_t g;
        dyn_gk_csr_t c;
        dyn_gk_pr_opts_t o = { 0.85, 1e-13, 1000, NULL, 0 };
        double x[MAXN], want[MAXN], worst = 0.0, sum = 0.0;
        uint32_t n = 1 + rnd(MAXN), i;
        int rc;
        make(&g, n, rnd(70), rnd(2), rnd(2), 1);
        to_csr(&g, 1, &c);
        rc = dyn_gk_pagerank(&c, &o, x, NULL);
        dense_pagerank(&g, 0.85, 1e-15, want);
        for (i = 0; i < n; i++) {
            if (fabs(x[i] - want[i]) > worst)
                worst = fabs(x[i] - want[i]);
            sum += x[i];
        }
        CHECK(rc == DYN_GK_OK && worst < 1e-11 && fabs(sum - 1.0) < 1e-12, "graph %d: PageRank equals the dense power iteration (worst %g, sum %.17g, rc %d)", t, worst, sum, rc);
        dyn_gk_csr_free(&c);
    }
    {
        el_t g;
        dyn_gk_csr_t c;
        double p[MAXN] = { 0 }, x[MAXN];
        dyn_gk_pr_opts_t o = { 0.85, 1e-12, 1000, p, 0 };
        make(&g, 10, 30, 1, 0, 0);
        to_csr(&g, 0, &c);
        p[3] = 2.0;
        CHECK(dyn_gk_pagerank(&c, &o, x, NULL) == DYN_GK_OK && x[3] > 0.15, "personalized PageRank concentrates on the seed (%g)", x[3]);
        p[3] = 0.0;
        CHECK(dyn_gk_pagerank(&c, &o, x, NULL) == DYN_GK_E_BAD_INPUT, "an all-zero personalization is refused");
        p[3] = -1.0;
        CHECK(dyn_gk_pagerank(&c, &o, x, NULL) == DYN_GK_E_BAD_INPUT, "a negative personalization is refused");
        o.pers = NULL;
        o.max_iter = 2;
        o.tol = 1e-300;
        CHECK(dyn_gk_pagerank(&c, &o, x, NULL) == DYN_GK_E_NO_CONVERGE && o.iters == 2, "non-convergence is reported after max_iter");
        dyn_gk_csr_free(&c);
        dyn_gk_csr_alloc(&c, 0, 0, 0);
        o.max_iter = 100;
        CHECK(dyn_gk_pagerank(&c, &o, x, NULL) == DYN_GK_OK && o.iters == 0, "the empty graph has an empty PageRank");
        dyn_gk_csr_free(&c);
    }
}

static void test_triangles_cores(void)
{
    int t;
    for (t = 0; t < 200; t++) {
        el_t g;
        dyn_gk_csr_t c, s;
        uint8_t a[MAXN][MAXN];
        double per[MAXN], total, local[MAXN], avg, trans, bt = 0.0, num = 0.0, den = 0.0, asum = 0.0;
        int32_t core[MAXN], naive[MAXN];
        uint32_t n = 1 + rnd(MAXN), i, j, k;
        int ok = 1, alive[MAXN], deg[MAXN], left;
        make(&g, n, rnd(120), rnd(2), 0, 1);
        to_csr(&g, 0, &c);
        dyn_gk_csr_sym(&c, 0, 0, &s);
        adj_simple(&g, 1, a);
        CHECK(dyn_gk_triangles(&s, per, &total, NULL) == DYN_GK_OK, "triangles run");
        dyn_gk_clustering(&s, per, local, &avg, &trans);
        for (i = 0; i < n; i++) {
            double ti = 0.0, d = 0.0;
            for (j = 0; j < n; j++) {
                d += a[i][j];
                for (k = j + 1; k < n; k++)
                    if (a[i][j] && a[i][k] && a[j][k])
                        ti++;
            }
            if (ti != per[i])
                ok = 0;
            bt += ti;
            num += 2 * ti;
            den += d * (d - 1);
            if (fabs((d > 1 ? 2 * ti / (d * (d - 1)) : 0.0) - local[i]) > 1e-15)
                ok = 0;
            asum += d > 1 ? 2 * ti / (d * (d - 1)) : 0.0;
        }
        CHECK(ok && total * 3 == bt, "graph %d: per-vertex triangles and local clustering equal the triple enumeration", t);
        CHECK(fabs(avg - asum / n) < 1e-15 && fabs(trans - (num > 0 ? num / den : 0.0)) < 1e-15, "graph %d: average clustering and transitivity", t);
        dyn_gk_core_numbers(&s, core);
        for (i = 0; i < n; i++) {
            alive[i] = 1;
            deg[i] = 0;
            for (j = 0; j < n; j++)
                deg[i] += a[i][j];
        }
        left = (int)n;
        for (k = 0; left; k++) {
            int removed = 1;
            while (removed) {
                removed = 0;
                for (i = 0; i < n; i++)
                    if (alive[i] && deg[i] < (int)k) {
                        alive[i] = 0;
                        naive[i] = (int32_t)k - 1;
                        left--;
                        removed = 1;
                        for (j = 0; j < n; j++)
                            if (a[i][j])
                                deg[j]--;
                    }
            }
        }
        ok = 1;
        for (i = 0; i < n; i++)
            if (core[i] != naive[i])
                ok = 0;
        CHECK(ok, "graph %d: core numbers equal naive peeling", t);
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&s);
    }
}

static void brute_bc(const el_t* g, int weighted, double* bc)
{
    uint32_t n = g->n, i, s, t, v;
    static double D[MAXN][MAXN], S[MAXN][MAXN];
    for (s = 0; s < n; s++)
        for (t = 0; t < n; t++) {
            D[s][t] = s == t ? 0.0 : INFINITY;
            S[s][t] = s == t ? 1.0 : 0.0;
        }
    for (i = 0; i < g->m; i++) {
        uint32_t a = g->u[i], b = g->v[i];
        double w = weighted ? g->w[i] : 1.0;
        if (a == b)
            continue;
        if (w < D[a][b])
            D[a][b] = w;
        if (!g->directed && w < D[b][a])
            D[b][a] = w;
    }
    for (v = 0; v < n; v++)
        for (s = 0; s < n; s++)
            for (t = 0; t < n; t++)
                if (D[s][v] + D[v][t] < D[s][t])
                    D[s][t] = D[s][v] + D[v][t];
    for (s = 0; s < n; s++) {
        uint32_t order[MAXN], cnt = 0, x, y;
        for (t = 0; t < n; t++)
            if (D[s][t] < INFINITY)
                order[cnt++] = t;
        for (x = 0; x < cnt; x++)
            for (y = x + 1; y < cnt; y++)
                if (D[s][order[y]] < D[s][order[x]]) {
                    uint32_t tmp = order[x];
                    order[x] = order[y];
                    order[y] = tmp;
                }
        for (x = 1; x < cnt; x++) {
            uint32_t tt = order[x];
            double sg = 0.0;
            for (y = 0; y < x; y++) {
                uint32_t p = order[y];
                double w = INFINITY;
                for (i = 0; i < g->m; i++) {
                    double wi = weighted ? g->w[i] : 1.0;
                    if (g->u[i] == g->v[i])
                        continue;
                    if (((g->u[i] == p && g->v[i] == tt) || (!g->directed && g->v[i] == p && g->u[i] == tt)) && wi < w)
                        w = wi;
                }
                if (w < INFINITY && D[s][p] + w == D[s][tt])
                    sg += S[s][p];
            }
            S[s][tt] = sg;
        }
    }
    for (v = 0; v < n; v++) {
        double b = 0.0;
        for (s = 0; s < n; s++)
            for (t = 0; t < n; t++)
                if (s != t && s != v && t != v && D[s][t] < INFINITY && D[s][v] + D[v][t] == D[s][t])
                    b += S[s][v] * S[v][t] / S[s][t];
        bc[v] = b;
    }
}

static void test_betweenness_closeness(void)
{
    int t;
    for (t = 0; t < 150; t++) {
        el_t g;
        dyn_gk_csr_t c, d, r;
        double bc[MAXN], want[MAXN], cl[MAXN], worst = 0.0;
        uint32_t n = 1 + rnd(MAXN), src[MAXN], i, s;
        int weighted = rnd(2), ok = 1;
        make(&g, n, rnd(60), rnd(2), weighted, 1);
        to_csr(&g, weighted, &c);
        dyn_gk_csr_dedup(&c, weighted, &d);
        for (i = 0; i < n; i++)
            src[i] = i;
        CHECK(dyn_gk_betweenness(&d, src, n, 0, bc, NULL) == DYN_GK_OK, "betweenness runs");
        brute_bc(&g, weighted, want);
        for (i = 0; i < n; i++)
            if (fabs(bc[i] - want[i]) > worst)
                worst = fabs(bc[i] - want[i]);
        CHECK(worst < 1e-9, "graph %d (%s, %s): Brandes equals pair counting (worst %g)", t, g.directed ? "directed" : "undirected", weighted ? "weighted" : "unweighted", worst);
        dyn_gk_csr_transpose(&d, 1, &r);
        dyn_gk_closeness(&r, 1, cl, NULL);
        for (s = 0; s < n; s++) {
            static double D[MAXN][MAXN];
            uint32_t a, b, k, reach = 0;
            double tot = 0.0, cc = 0.0;
            for (a = 0; a < n; a++)
                for (b = 0; b < n; b++)
                    D[a][b] = a == b ? 0.0 : INFINITY;
            for (i = 0; i < g.m; i++) {
                double w = weighted ? g.w[i] : 1.0;
                if (g.u[i] == g.v[i])
                    continue;
                if (w < D[g.u[i]][g.v[i]])
                    D[g.u[i]][g.v[i]] = w;
                if (!g.directed && w < D[g.v[i]][g.u[i]])
                    D[g.v[i]][g.u[i]] = w;
            }
            for (k = 0; k < n; k++)
                for (a = 0; a < n; a++)
                    for (b = 0; b < n; b++)
                        if (D[a][k] + D[k][b] < D[a][b])
                            D[a][b] = D[a][k] + D[k][b];
            for (a = 0; a < n; a++)
                if (D[a][s] < INFINITY) {
                    reach++;
                    tot += D[a][s];
                }
            if (tot > 0 && n > 1)
                cc = (reach - 1.0) / tot * ((reach - 1.0) / (n - 1.0));
            if (fabs(cc - cl[s]) > 1e-12)
                ok = 0;
        }
        CHECK(ok, "graph %d: closeness equals all-pairs distances to the vertex", t);
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&d);
        dyn_gk_csr_free(&r);
    }
    {
        double bc[4] = { 1, 2, 3, 4 };
        uint32_t src[2] = { 0, 2 };
        dyn_gk_betweenness_rescale(bc, 4, 1, 1, 0, src, 2, 1);
        CHECK(fabs(bc[0] - 1.0 / 2) < 1e-15 && fabs(bc[1] - 2.0 / 4) < 1e-15 && fabs(bc[2] - 3.0 / 2) < 1e-15, "sampled rescale splits source and non-source scales");
    }
}

static void test_components(void)
{
    int t;
    for (t = 0; t < 200; t++) {
        el_t g;
        dyn_gk_csr_t c;
        static uint8_t R[MAXN][MAXN];
        int32_t comp[MAXN], wc[MAXN], lv[MAXN];
        uint32_t n = 1 + rnd(MAXN), nc, nw, nl, i, j, k;
        int ok = 1, rc;
        make(&g, n, rnd(50), 1, 0, rnd(4) == 0);
        to_csr(&g, 0, &c);
        dyn_gk_scc(&c, comp, &nc);
        memset(R, 0, sizeof R);
        for (i = 0; i < n; i++)
            R[i][i] = 1;
        for (i = 0; i < g.m; i++)
            R[g.u[i]][g.v[i]] = 1;
        for (k = 0; k < n; k++)
            for (i = 0; i < n; i++)
                for (j = 0; j < n; j++)
                    if (R[i][k] && R[k][j])
                        R[i][j] = 1;
        for (i = 0; i < n; i++)
            for (j = 0; j < n; j++) {
                if ((comp[i] == comp[j]) != (R[i][j] && R[j][i]))
                    ok = 0;
            }
        for (i = 0; i < g.m; i++)
            if (comp[g.u[i]] < comp[g.v[i]])
                ok = 0;
        CHECK(ok, "graph %d: SCC is mutual reachability; arcs never climb the reverse-topological ids", t);
        dyn_gk_wcc(&c, wc, &nw);
        ok = 1;
        for (i = 0; i < g.m; i++)
            if (wc[g.u[i]] != wc[g.v[i]])
                ok = 0;
        for (i = 0, k = 0; i < n; i++) {
            if ((uint32_t)wc[i] > k)
                ok = 0;
            if ((uint32_t)wc[i] == k)
                k++;
        }
        CHECK(ok && k == nw, "graph %d: WCC labels are first-appearance ordered and respect every arc", t);
        rc = dyn_gk_topo_levels(&c, lv, &nl);
        {
            int acyclic = 1;
            for (i = 0; i < n; i++)
                for (j = 0; j < n; j++)
                    if (i != j && R[i][j] && R[j][i])
                        acyclic = 0;
            for (i = 0; i < g.m; i++)
                if (g.u[i] == g.v[i])
                    acyclic = 0;
            CHECK(acyclic ? rc == DYN_GK_OK : rc == DYN_GK_E_CYCLE, "graph %d: levels refuse exactly the cyclic graphs", t);
            if (acyclic) {
                ok = 1;
                for (j = 0; j < n; j++) {
                    int32_t want = 0;
                    for (i = 0; i < g.m; i++)
                        if (g.v[i] == j && lv[g.u[i]] + 1 > want)
                            want = lv[g.u[i]] + 1;
                    if (lv[j] != want)
                        ok = 0;
                }
                CHECK(ok, "graph %d: level = 1 + max level of predecessors", t);
            }
        }
        dyn_gk_csr_free(&c);
    }
}

static void test_flow_matching(void)
{
    int t;
    for (t = 0; t < 150; t++) {
        el_t g;
        dyn_gk_csr_t c;
        double val, *fl, best = INFINITY;
        uint8_t side[MAXN];
        uint32_t n = 2 + rnd(9), mask;
        make(&g, n, rnd(30), rnd(2), 1, 1);
        to_csr(&g, 1, &c);
        fl = (double*)malloc((c.m + 1) * sizeof(double));
        CHECK(dyn_gk_maxflow(&c, 0, n - 1, &val, fl, side, NULL) == DYN_GK_OK, "maxflow runs");
        for (mask = 0; mask < (1u << (n - 2)); mask++) {
            double cut = 0.0;
            uint32_t a, e;
            for (a = 0; a < n; a++) {
                int ina = a == 0 || (a != n - 1 && ((mask >> (a - 1)) & 1));
                for (e = c.off[a]; e < c.off[a + 1]; e++) {
                    uint32_t b = c.tgt[e];
                    int inb = b == 0 || (b != n - 1 && ((mask >> (b - 1)) & 1));
                    if (ina && !inb)
                        cut += c.w[e];
                }
            }
            if (cut < best)
                best = cut;
        }
        {
            double sc = 0.0, net = 0.0;
            uint32_t a, e;
            int cons = 1;
            for (a = 0; a < n; a++)
                for (e = c.off[a]; e < c.off[a + 1]; e++)
                    if (side[a] && !side[c.tgt[e]])
                        sc += c.w[e];
            for (a = 1; a + 1 < n; a++) {
                double bal = 0.0;
                uint32_t x;
                for (x = 0; x < n; x++)
                    for (e = c.off[x]; e < c.off[x + 1]; e++) {
                        if (x == a)
                            bal -= fl[e];
                        if (c.tgt[e] == a)
                            bal += fl[e];
                    }
                if (fabs(bal) > 1e-9)
                    cons = 0;
            }
            for (e = c.off[0]; e < c.off[1]; e++)
                net += fl[e];
            for (a = 0; a < n; a++)
                for (e = c.off[a]; e < c.off[a + 1]; e++)
                    if (c.tgt[e] == 0)
                        net -= fl[e];
            CHECK(fabs(val - best) < 1e-9 && fabs(sc - best) < 1e-9 && side[0] && !side[n - 1] && cons && fabs(net - val) < 1e-9,
                "graph %d: max-flow %g equals the brute min cut %g; the reported cut and flows are consistent", t, val, best);
        }
        free(fl);
        dyn_gk_csr_free(&c);
    }
    for (t = 0; t < 150; t++) {
        el_t g;
        dyn_gk_csr_t c, s;
        uint8_t left[MAXN];
        int32_t mate[MAXN];
        uint32_t n = 1 + rnd(12), nl = n / 2, i, size, bestm = 0;
        uint32_t eu[64], ev[64], ne = 0, mask;
        g.n = n;
        g.m = 0;
        g.directed = 0;
        for (i = 0; i < 25 && nl > 0 && n > nl; i++) {
            g.u[g.m] = rnd(nl);
            g.v[g.m] = nl + rnd(n - nl);
            g.w[g.m] = 1.0;
            g.m++;
        }
        to_csr(&g, 0, &c);
        dyn_gk_csr_sym(&c, 0, 0, &s);
        for (i = 0; i < n; i++)
            left[i] = i < nl;
        CHECK(dyn_gk_matching(&s, left, mate, &size) == DYN_GK_OK, "matching runs");
        for (i = 0; i < n; i++) {
            uint32_t e;
            for (e = s.off[i]; e < s.off[i + 1]; e++)
                if (i < s.tgt[e]) {
                    eu[ne] = i;
                    ev[ne] = s.tgt[e];
                    ne++;
                }
        }
        for (mask = 0; mask < (1u << (ne < 16 ? ne : 16)); mask++) {
            uint32_t used = 0, cnt = 0, b;
            int okm = 1;
            for (b = 0; b < ne && b < 16; b++)
                if ((mask >> b) & 1) {
                    if ((used >> eu[b]) & 1 || (used >> ev[b]) & 1)
                        okm = 0;
                    used |= (1u << eu[b]) | (1u << ev[b]);
                    cnt++;
                }
            if (okm && cnt > bestm)
                bestm = cnt;
        }
        if (ne <= 16) {
            int ok = 1;
            for (i = 0; i < n; i++)
                if (mate[i] >= 0 && mate[mate[i]] != (int32_t)i)
                    ok = 0;
            CHECK(ok && size == bestm, "graph %d: Hopcroft-Karp finds a maximum matching (%u vs %u)", t, size, bestm);
        }
        {
            uint8_t l2[MAXN];
            CHECK(dyn_gk_bipartition(&s, l2) == DYN_GK_OK, "a bipartite graph is 2-colourable");
        }
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&s);
    }
    {
        el_t g = { 3, 3, { 0, 1, 2 }, { 1, 2, 0 }, { 1, 1, 1 }, 0 };
        dyn_gk_csr_t c, s;
        uint8_t l2[3];
        to_csr(&g, 0, &c);
        dyn_gk_csr_sym(&c, 0, 0, &s);
        CHECK(dyn_gk_bipartition(&s, l2) == DYN_GK_E_NOT_BIPARTITE, "a triangle is not bipartite");
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&s);
    }
}

static double brute_q(const el_t* g, const int32_t* comm, double gamma)
{
    static double A[MAXN][MAXN];
    double k[MAXN] = { 0 }, m2 = 0.0, q = 0.0;
    uint32_t i, j;
    memset(A, 0, sizeof A);
    for (i = 0; i < g->m; i++) {
        A[g->u[i]][g->v[i]] += g->w[i];
        A[g->v[i]][g->u[i]] += g->w[i];
    }
    for (i = 0; i < g->n; i++)
        for (j = 0; j < g->n; j++) {
            k[i] += A[i][j];
            m2 += A[i][j];
        }
    if (m2 == 0.0)
        return 0.0;
    for (i = 0; i < g->n; i++)
        for (j = 0; j < g->n; j++)
            if (comm[i] == comm[j])
                q += A[i][j] - gamma * k[i] * k[j] / m2;
    return q / m2;
}

static void test_communities(void)
{
    int t;
    for (t = 0; t < 150; t++) {
        el_t g;
        dyn_gk_csr_t c, a, s;
        int32_t comm[MAXN], lab[MAXN], single[MAXN];
        dyn_gk_louvain_opts_t o = { 1.0, 1e-7, (uint64_t)t, 0, 0.0 };
        uint32_t n = 1 + rnd(MAXN), nc, nl, i;
        double q, q1, qs;
        int dir = (int)rnd(2);
        make(&g, n, rnd(70), dir, rnd(2), 1);
        to_csr(&g, 1, &c);
        dyn_gk_csr_sym(&c, dir ? 1 : 2, 1, &a);
        CHECK(dyn_gk_louvain(&a, &o, comm, &nc, NULL) == DYN_GK_OK, "louvain runs");
        q = brute_q(&g, comm, 1.0);
        if (!dir)
            dyn_gk_modularity(&c, 0, comm, 1.0, &q);
        q1 = brute_q(&g, comm, 1.0);
        for (i = 0; i < n; i++)
            single[i] = (int32_t)i;
        qs = brute_q(&g, single, 1.0);
        CHECK(fabs(q - q1) < 1e-12 && fabs(o.q - q1) < 1e-12 && q1 >= qs - 1e-12, "graph %d: Louvain modularity %.15g (modularity() %.15g) equals the definition %.15g and beats singletons (levels %u)", t, o.q, q, q1, o.levels);
        {
            int ok = 1;
            for (i = 0; i < n; i++)
                if (comm[i] < 0 || (uint32_t)comm[i] >= nc)
                    ok = 0;
            CHECK(ok, "graph %d: community ids are dense", t);
        }
        if (!dir) {
            dyn_gk_modularity(&c, 0, comm, 0.5, &q);
            CHECK(fabs(q - brute_q(&g, comm, 0.5)) < 1e-12, "graph %d: resolution enters as gamma", t);
        }
        dyn_gk_csr_sym(&c, 0, 0, &s);
        dyn_gk_lpa(&s, (uint64_t)t, 1000, lab, &nl);
        {
            int ok = 1;
            uint32_t u, e;
            for (u = 0; u < n; u++) {
                double cnt[MAXN] = { 0 }, top = 0.0;
                if (s.off[u] == s.off[u + 1])
                    continue;
                for (e = s.off[u]; e < s.off[u + 1]; e++)
                    cnt[lab[s.tgt[e]]] += 1.0;
                for (i = 0; i < n; i++)
                    if (cnt[i] > top)
                        top = cnt[i];
                if (cnt[lab[u]] != top)
                    ok = 0;
            }
            CHECK(ok, "graph %d: label propagation stops only where every label is a most-frequent neighbour label", t);
        }
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&a);
        dyn_gk_csr_free(&s);
    }
    {
        el_t g;
        dyn_gk_csr_t c;
        int32_t comm[8];
        double q;
        uint32_t i;
        make(&g, 8, 20, 1, 1, 1);
        to_csr(&g, 1, &c);
        for (i = 0; i < 8; i++)
            comm[i] = (int32_t)(i % 3);
        dyn_gk_modularity(&c, 1, comm, 1.0, &q);
        {
            double m = 0, kin[3] = { 0 }, kout[3] = { 0 }, L[3] = { 0 }, want = 0;
            for (i = 0; i < g.m; i++) {
                m += g.w[i];
                kout[comm[g.u[i]]] += g.w[i];
                kin[comm[g.v[i]]] += g.w[i];
                if (comm[g.u[i]] == comm[g.v[i]])
                    L[comm[g.u[i]]] += g.w[i];
            }
            for (i = 0; i < 3; i++)
                want += L[i] / m - kout[i] * kin[i] / (m * m);
            CHECK(fabs(q - want) < 1e-12, "directed modularity follows Leicht-Newman");
        }
        comm[0] = 8;
        CHECK(dyn_gk_modularity(&c, 1, comm, 1.0, &q) == DYN_GK_E_BAD_INPUT, "a community id >= n is refused");
        dyn_gk_csr_free(&c);
    }
}

static void test_link(void)
{
    int t;
    for (t = 0; t < 100; t++) {
        el_t g;
        dyn_gk_csr_t c, s;
        uint8_t a[MAXN][MAXN];
        uint32_t n = 2 + rnd(MAXN - 2), pairs[40], i, m;
        double out[5][20];
        int ok = 1;
        make(&g, n, rnd(80), rnd(2), 0, 1);
        to_csr(&g, 0, &c);
        dyn_gk_csr_sym(&c, 0, 0, &s);
        adj_simple(&g, 1, a);
        for (i = 0; i < 40; i++)
            pairs[i] = rnd(n);
        for (m = 0; m < 5; m++)
            dyn_gk_link_scores(&s, pairs, 20, (int)m, out[m]);
        for (i = 0; i < 20; i++) {
            uint32_t u = pairs[2 * i], v = pairs[2 * i + 1], w;
            double cn = 0, uni = 0, aa = 0, ra = 0, du = 0, dv = 0;
            for (w = 0; w < n; w++) {
                double dw = 0;
                uint32_t x;
                for (x = 0; x < n; x++)
                    dw += a[w][x];
                du += a[u][w];
                dv += a[v][w];
                if (a[u][w] || a[v][w])
                    uni++;
                if (a[u][w] && a[v][w] && w != u && w != v) {
                    cn++;
                    aa += 1 / log(dw);
                    ra += 1 / dw;
                }
            }
            if (out[DYN_GK_LINK_COMMON][i] != cn || fabs(out[DYN_GK_LINK_JACCARD][i] - (uni ? cn / uni : 0)) > 1e-15 || fabs(out[DYN_GK_LINK_ADAMIC_ADAR][i] - aa) > 1e-12 || fabs(out[DYN_GK_LINK_RESOURCE][i] - ra) > 1e-12 || out[DYN_GK_LINK_PREFERENTIAL][i] != du * dv)
                ok = 0;
        }
        CHECK(ok, "graph %d: link scores equal their set definitions", t);
        pairs[0] = n;
        CHECK(dyn_gk_link_scores(&s, pairs, 20, 0, out[0]) == DYN_GK_E_BAD_INPUT, "an out-of-range pair is refused");
        dyn_gk_csr_free(&c);
        dyn_gk_csr_free(&s);
    }
}

static void big_graph(dyn_gk_csr_t* c, uint32_t n, uint32_t deg)
{
    uint32_t u, k, at = 0;
    dyn_gk_csr_alloc(c, n, (uint64_t)n * deg, 1);
    for (u = 0; u < n; u++) {
        c->off[u] = at;
        for (k = 0; k < deg; k++) {
            uint32_t v = (u / 64) * 64 + rnd(64);
            if (rnd(10) == 0)
                v = rnd(n);
            c->tgt[at] = v < n ? v : n - 1;
            c->w[at] = 1.0 + rnd(5);
            at++;
        }
    }
    c->off[n] = at;
}

static void test_determinism(void)
{
    dyn_gk_csr_t c, s, d, r;
    uint32_t n = 40000, *src, i, k = 64;
    double *x1, *x2, *b1, *b2, *t1, *t2, *cl1, *cl2, tot1, tot2;
    dyn_gk_pr_opts_t o1 = { 0.85, 1e-10, 200, NULL, 0 }, o2 = o1;
    int same = 1;
    dyn_par_stats_t st0, st1;
    big_graph(&c, n, 8);
    dyn_gk_csr_sym(&c, 0, 0, &s);
    dyn_gk_csr_dedup(&c, 0, &d);
    dyn_gk_csr_transpose(&d, 0, &r);
    x1 = malloc(n * sizeof(double));
    x2 = malloc(n * sizeof(double));
    b1 = malloc(n * sizeof(double));
    b2 = malloc(n * sizeof(double));
    t1 = malloc(n * sizeof(double));
    t2 = malloc(n * sizeof(double));
    cl1 = malloc(2000 * sizeof(double));
    cl2 = malloc(2000 * sizeof(double));
    src = xmalloc(k * sizeof(uint32_t));
    for (i = 0; i < k; i++)
        src[i] = rnd(n);
    dyn_par_set_spawn_limit(0);
    dyn_gk_pagerank(&c, &o1, x1, NULL);
    dyn_gk_betweenness(&d, src, k, 0, b1, NULL);
    dyn_gk_triangles(&s, t1, &tot1, NULL);
    dyn_par_set_spawn_limit(-1);
    dyn_par_stats(&st0);
    dyn_gk_pagerank(&c, &o2, x2, NULL);
    dyn_gk_betweenness(&d, src, k, 0, b2, NULL);
    dyn_gk_triangles(&s, t2, &tot2, NULL);
    dyn_par_stats(&st1);
    CHECK(dyn_pool_max_threads() < 2 || st1.spawned >= st0.spawned + 3, "the default run really used helper threads (%llu spawned)", (unsigned long long)(st1.spawned - st0.spawned));
    for (i = 0; i < n; i++)
        if (memcmp(&x1[i], &x2[i], 8) || memcmp(&b1[i], &b2[i], 8) || t1[i] != t2[i])
            same = 0;
    CHECK(same && o1.iters == o2.iters && tot1 == tot2, "PageRank, sampled betweenness and triangles are bit-identical at 1 thread and at the default count");
    {
        dyn_gk_csr_t cs, rs2;
        uint32_t m2 = 0, u, e;
        dyn_gk_csr_alloc(&cs, 2000, d.off[2000], 0);
        for (u = 0; u < 2000; u++) {
            cs.off[u] = m2;
            for (e = d.off[u]; e < d.off[u + 1]; e++)
                if (d.tgt[e] < 2000)
                    cs.tgt[m2++] = d.tgt[e];
        }
        cs.off[2000] = m2;
        cs.m = m2;
        dyn_gk_csr_transpose(&cs, 0, &rs2);
        dyn_par_set_spawn_limit(0);
        dyn_gk_closeness(&rs2, 1, cl1, NULL);
        dyn_par_set_spawn_limit(-1);
        dyn_gk_closeness(&rs2, 1, cl2, NULL);
        CHECK(memcmp(cl1, cl2, 2000 * sizeof(double)) == 0, "closeness is bit-identical across thread counts");
        dyn_gk_csr_free(&cs);
        dyn_gk_csr_free(&rs2);
    }
    free(x1);
    free(x2);
    free(b1);
    free(b2);
    free(t1);
    free(t2);
    free(cl1);
    free(cl2);
    free(src);
    dyn_gk_csr_free(&c);
    dyn_gk_csr_free(&s);
    dyn_gk_csr_free(&d);
    dyn_gk_csr_free(&r);
}

static int stop_after;

static int stop_cb(void* ud)
{
    (void)ud;
    return --stop_after < 0;
}

static void test_cancel_and_edges(void)
{
    dyn_gk_csr_t c, e0;
    dyn_gk_ctl_t ctl = { stop_cb, NULL };
    dyn_gk_pr_opts_t o = { 0.85, 1e-300, 100000, NULL, 0 };
    double *x, val;
    uint32_t src[4] = { 0, 1, 2, 3 }, nc;
    int32_t comp[1];
    big_graph(&c, 5000, 4);
    x = malloc(5000 * sizeof(double));
    stop_after = 3;
    CHECK(dyn_gk_pagerank(&c, &o, x, &ctl) == DYN_GK_E_CANCELLED, "a stop request ends PageRank");
    stop_after = 0;
    CHECK(dyn_gk_betweenness(&c, src, 4, 0, x, &ctl) == DYN_GK_E_CANCELLED, "a stop request ends betweenness");
    stop_after = 0;
    CHECK(dyn_gk_maxflow(&c, 0, 1, &val, NULL, NULL, &ctl) == DYN_GK_E_CANCELLED, "a stop request ends max-flow");
    CHECK(dyn_gk_maxflow(&c, 0, 0, &val, NULL, NULL, NULL) == DYN_GK_E_BAD_INPUT, "source == sink is refused");
    dyn_gk_csr_alloc(&e0, 0, 0, 0);
    CHECK(dyn_gk_scc(&e0, comp, &nc) == DYN_GK_OK && nc == 0, "the empty graph has no components");
    CHECK(dyn_gk_csr_validate(&e0) == DYN_GK_OK, "the empty CSR validates");
    c.tgt[0] = 5000;
    CHECK(dyn_gk_csr_validate(&c) == DYN_GK_E_BAD_INPUT, "a target past n is refused");
    c.tgt[0] = 0;
    c.off[3] = c.off[4] + 1;
    CHECK(dyn_gk_csr_validate(&c) == DYN_GK_E_BAD_INPUT, "decreasing offsets are refused");
    free(x);
    dyn_gk_csr_free(&c);
    dyn_gk_csr_free(&e0);
}

int main(void)
{
    test_csr_helpers();
    test_pagerank();
    test_triangles_cores();
    test_betweenness_closeness();
    test_components();
    test_flow_matching();
    test_communities();
    test_link();
    test_determinism();
    test_cancel_and_edges();
    printf("test_graphk_analytics: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
