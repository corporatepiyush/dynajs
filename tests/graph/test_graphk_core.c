/*
 * test_graphk_core.c -- src/core/graphk (the dyna:structures Graph storage
 * and algorithms) as a standalone C library: no engine, visitors are plain
 * C callbacks.
 *
 * External oracles (Cormen, Leiserson, Rivest, Stein, Introduction to
 * Algorithms, 3rd ed.):
 *  - Fig. 24.6 Dijkstra example: d(s,t,x,y,z) = 0, 8, 9, 5, 7.
 *  - Fig. 24.4 Bellman-Ford example with negative edges:
 *    d(s,t,x,y,z) = 0, 2, 4, 7, -2.
 *  - Fig. 25.4 Floyd-Warshall example: the final D(5) matrix.
 *  - Fig. 23.1 MST example (a..i): total weight 37, 8 edges.
 *  - Sec. 24.2 DAG shortest paths, run as longest paths (critical path):
 *    on Fig. 24.5's DAG (r,s,t,x,y,z; weights r-s 5, r-t 3, s-t 2, s-x 6,
 *    t-x 7, t-y 4, t-z 2, x-y -1, x-z 1, y-z -2) the longest path is
 *    r,s,t,x,z of length 15.
 * The kernels read a dyn_gk_csr_t; C() below rebuilds one from the
 * adjacency for every call, so a graph mutated between calls is seen fresh.
 * Properties checked over 400 seeded random graphs: Dijkstra, Bellman-Ford,
 * Floyd-Warshall row and A* with a zero heuristic agree when no weight is
 * negative; every topological order respects every edge; BFS reaches the
 * same set as the connected component of the source (undirected); every
 * MST is a spanning forest of the component count.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-graphk.h"
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

typedef struct {
    int a, b;
    double w;
} edge_t;

static dyn_graph_t* build(int directed, int weighted, uint32_t n, const edge_t* es, size_t ne)
{
    dyn_graph_t* g = dyn_gk_new(directed, weighted);
    size_t i;
    if (n)
        dyn_gk_ensure(g, n - 1);
    for (i = 0; i < ne; i++)
        dyn_gk_add_edge(g, (uint32_t)es[i].a, (uint32_t)es[i].b, es[i].w);
    return g;
}

static dyn_gk_csr_t ring[4];
static unsigned ring_at;

static const dyn_gk_csr_t* C(const dyn_graph_t* g)
{
    dyn_gk_csr_t* c = &ring[ring_at++ % 4];
    dyn_gk_csr_free(c);
    if (dyn_gk_csr_of_graph(g, 1, c) < 0) {
        printf("FAIL csr build\n");
        exit(1);
    }
    return c;
}

static void ring_free(void)
{
    unsigned k;
    for (k = 0; k < 4; k++)
        dyn_gk_csr_free(&ring[k]);
}

static void test_clrs(void)
{
    enum { S,
        T,
        X,
        Y,
        Z };
    static const edge_t dij[] = { { S, T, 10 }, { S, Y, 5 }, { T, X, 1 }, { T, Y, 2 }, { Y, T, 3 }, { Y, X, 9 }, { Y, Z, 2 }, { X, Z, 4 }, { Z, X, 6 }, { Z, S, 7 } };
    static const edge_t bf[] = { { S, T, 6 }, { S, Y, 7 }, { T, X, 5 }, { T, Y, 8 }, { T, Z, -4 }, { X, T, -2 }, { Y, X, -3 }, { Y, Z, 9 }, { Z, S, 2 }, { Z, X, 7 } };
    static const edge_t fw[] = { { 0, 1, 3 }, { 0, 2, 8 }, { 0, 4, -4 }, { 1, 3, 1 }, { 1, 4, 7 }, { 2, 1, 4 }, { 3, 0, 2 }, { 3, 2, -5 }, { 4, 3, 6 } };
    static const double fwd[25] = { 0, 1, -3, 2, -4, 3, 0, -4, 1, -1, 7, 4, 0, 5, 3, 2, -1, -5, 0, -2, 8, 5, 1, 6, 0 };
    static const edge_t mst[] = { { 0, 1, 4 }, { 0, 7, 8 }, { 1, 2, 8 }, { 1, 7, 11 }, { 2, 3, 7 }, { 2, 5, 4 }, { 2, 8, 2 }, { 3, 4, 9 }, { 3, 5, 14 }, { 4, 5, 10 }, { 5, 6, 2 }, { 6, 7, 1 }, { 6, 8, 6 }, { 7, 8, 7 } };
    dyn_graph_t* g;
    double d[5], *m = NULL, total = 0, ad = 0;
    int* path = NULL;
    uint32_t plen = 0, ne = 0;
    dyn_gk_edge_t* tree = NULL;
    int rc, i;

    g = build(1, 1, 5, dij, 10);
    rc = dyn_gk_dijkstra(C(g), S, d, NULL);
    CHECK(rc == 0 && d[S] == 0 && d[T] == 8 && d[X] == 9 && d[Y] == 5 && d[Z] == 7, "CLRS 24.6 dijkstra rc=%d %g %g %g %g %g", rc, d[0], d[1], d[2], d[3], d[4]);
    rc = dyn_gk_bellman_ford(C(g), S, d);
    CHECK(rc == 0 && d[T] == 8 && d[X] == 9, "bellman-ford agrees on 24.6");
    {
        static const double zero[5] = { 0, 0, 0, 0, 0 };
        rc = dyn_gk_astar(C(g), S, X, zero, &ad, &path, &plen);
        CHECK(rc == 0 && ad == 9 && plen == 4 && path[0] == S && path[1] == Y && path[2] == T && path[3] == X, "A* s->x is s,y,t,x at 9 (rc=%d len=%u)", rc, plen);
        free(path);
    }
    dyn_gk_free(g);

    g = build(1, 1, 5, bf, 10);
    CHECK(dyn_gk_dijkstra(C(g), S, d, NULL) == DYN_GK_E_NEG_WEIGHT, "dijkstra refuses a negative weight");
    CHECK(dyn_gk_has_negative(g) == 1, "has_negative sees -4");
    rc = dyn_gk_bellman_ford(C(g), S, d);
    CHECK(rc == 0 && d[S] == 0 && d[T] == 2 && d[X] == 4 && d[Y] == 7 && d[Z] == -2, "CLRS 24.4 bellman-ford rc=%d %g %g %g %g %g", rc, d[0], d[1], d[2], d[3], d[4]);
    dyn_gk_add_edge(g, X, Y, -1);
    CHECK(dyn_gk_bellman_ford(C(g), S, d) == DYN_GK_E_NEG_CYCLE, "x->y -1 closes the negative cycle x,y");
    dyn_gk_free(g);

    g = build(1, 1, 5, fw, 9);
    rc = dyn_gk_floyd(C(g), &m);
    {
        int same = rc == 0 && m;
        for (i = 0; same && i < 25; i++)
            same = m[i] == fwd[i];
        CHECK(same, "CLRS 25.4 floyd-warshall D(5) (rc=%d)", rc);
    }
    free(m);
    dyn_gk_add_edge(g, 2, 3, -1);
    CHECK(dyn_gk_floyd(C(g), &m) == DYN_GK_E_NEG_CYCLE && m == NULL, "floyd reports a negative cycle and returns no matrix");
    dyn_gk_free(g);

    g = build(0, 1, 9, mst, 14);
    rc = dyn_gk_mst(C(g), &tree, &ne, &total);
    CHECK(rc == 0 && ne == 8 && total == 37, "CLRS 23.1 MST weight 37 over 8 edges (rc=%d ne=%u w=%g)", rc, ne, total);
    CHECK(ne && tree[0].w == 1 && tree[ne - 1].w == 9, "Kruskal emits edges by ascending weight");
    {
        uint32_t k, lo = 1;
        for (k = 0; k < ne; k++)
            lo &= tree[k].u <= tree[k].v;
        CHECK(lo, "each MST edge is reported low id first");
    }
    free(tree);
    dyn_gk_free(g);
    {
        static const edge_t neg[] = { { 0, 1, 2 }, { 1, 2, -3 }, { 2, 3, -0.5 }, { 3, 0, 1 }, { 0, 2, -1 } };
        g = build(0, 1, 4, neg, 5);
        rc = dyn_gk_mst(C(g), &tree, &ne, &total);
        CHECK(rc == 0 && ne == 3 && total == -4.5 && tree[0].w == -3 && tree[1].w == -1 && tree[2].w == -0.5,
            "negative weights sort below zero (rc=%d w=%g)", rc, total);
        free(tree);
        dyn_gk_free(g);
        g = build(1, 1, 3, neg + 2, 1);
        CHECK(dyn_gk_dijkstra(C(g), 2, d, NULL) == DYN_GK_E_NEG_WEIGHT, "dijkstra refuses a lone -0.5 edge");
        dyn_gk_free(g);
    }
}

typedef struct {
    int seen[64];
    int n, stop_after, fail_at, mutate_at;
    dyn_graph_t* g;
} vis_t;

static int visit(void* ud, int node)
{
    vis_t* v = (vis_t*)ud;
    v->seen[v->n++] = node;
    if (v->n == v->fail_at)
        return -1;
    if (v->n == v->mutate_at) {
        dyn_gk_add_edge(v->g, 0, 0, 1);
        return DYN_GK_E_MUTATED;
    }
    return v->n < v->stop_after;
}

static void test_walks(void)
{
    static const edge_t es[] = { { 0, 1, 1 }, { 0, 2, 1 }, { 1, 3, 1 }, { 2, 3, 1 }, { 3, 4, 1 }, { 5, 6, 1 } };
    dyn_graph_t* g = build(1, 0, 7, es, 6);
    int order[8];
    int64_t no;
    vis_t v;
    uint32_t visited = 0;
    int rc;
    int32_t *off, *ed;
    uint64_t total;

    no = dyn_gk_bfs(C(g), 0, order);
    CHECK(no == 5 && order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3 && order[4] == 4, "bfs order 0 1 2 3 4 (n=%lld)", (long long)no);
    no = dyn_gk_dfs(C(g), 0, order);
    CHECK(no == 5 && order[0] == 0 && order[1] == 1 && order[2] == 3 && order[3] == 4 && order[4] == 2, "dfs order 0 1 3 4 2 (n=%lld)", (long long)no);
    no = dyn_gk_topo(C(g), order);
    CHECK(no == 7 && order[0] == 0 && order[1] == 5 && order[6] == 4, "Kahn order starts 0 5 and ends 4 (n=%lld)", (long long)no);

    memset(&v, 0, sizeof v);
    v.stop_after = 3;
    v.g = g;
    rc = dyn_gk_bfs_visit(C(g), 0, visit, &v, &visited);
    CHECK(rc == 0 && visited == 3 && v.n == 3, "bfs visitor stops after its third falsy answer (rc=%d visited=%u)", rc, visited);
    memset(&v, 0, sizeof v);
    v.stop_after = 99;
    rc = dyn_gk_dfs_visit(C(g), 0, visit, &v, &visited);
    CHECK(rc == 0 && visited == 5 && v.seen[2] == 3, "dfs visitor sees the dfs order");
    memset(&v, 0, sizeof v);
    v.stop_after = 99;
    v.fail_at = 2;
    CHECK(dyn_gk_topo_visit(C(g), visit, &v, &visited) == DYN_GK_E_CALLBACK, "a failing visitor aborts the walk");
    memset(&v, 0, sizeof v);
    v.stop_after = 99;
    v.mutate_at = 2;
    v.g = g;
    CHECK(dyn_gk_bfs_visit(C(g), 0, visit, &v, &visited) == DYN_GK_E_MUTATED && v.n == 2, "bfs passes on the visitor's report that it added an edge");
    CHECK(dyn_gk_topo(C(g), order) == DYN_GK_E_CYCLE, "the self loop the visitor added is a cycle");
    memset(&v, 0, sizeof v);
    v.stop_after = 99;
    CHECK(dyn_gk_topo_visit(C(g), visit, &v, &visited) == DYN_GK_E_CYCLE && v.n == 2, "topo visit walks 5, 6 then reports the cycle through 0 (%d)", v.n);

    rc = dyn_gk_export_csr(g, &off, &ed, &total);
    CHECK(rc == 0 && total == 7 && off[0] == 0 && off[1] == 3 && off[7] == 7 && ed[0] == 1 && ed[2] == 0, "export_csr offsets/edges (rc=%d total=%llu)", rc, (unsigned long long)total);
    free(off);
    free(ed);
    CHECK(dyn_gk_has_edge(g, 3, 4) && !dyn_gk_has_edge(g, 4, 3) && !dyn_gk_has_edge(g, 99, 0), "has_edge is directed and range-safe");
    dyn_gk_free(g);

    g = build(0, 0, 7, es, 6);
    {
        int comp[7];
        rc = dyn_gk_components(C(g), comp);
        CHECK(rc == 0 && comp[0] == 0 && comp[4] == 0 && comp[5] == 1 && comp[6] == 1, "two components {0..4} {5,6}");
    }
    CHECK(g->m == 6 && g->adj[3].n == 3, "undirected edges are stored both ways and counted once");
    dyn_gk_add_edge(g, 2, 2, 5);
    CHECK(g->m == 7 && g->adj[2].n == 3 && g->adj[2].e[2].w == 1.0, "a self loop is stored once; an unweighted graph keeps weight 1");
    dyn_gk_free(g);

    g = dyn_gk_new(1, 0);
    {
        double* m = (double*)1;
        dyn_gk_edge_t* t = (dyn_gk_edge_t*)1;
        uint32_t ne = 9;
        double w = 9;
        CHECK(dyn_gk_topo(C(g), order) == 0 && dyn_gk_floyd(C(g), &m) == 0 && dyn_gk_mst(C(g), &t, &ne, &w) == 0 && ne == 0 && w == 0,
            "an empty graph sorts, closes and spans to nothing");
        free(m);
        free(t);
    }
    dyn_gk_ensure(g, DYN_FLOYD_MAX_N);
    {
        double* m = NULL;
        CHECK(dyn_gk_floyd(C(g), &m) == DYN_GK_E_TOO_LARGE && !m, "floyd refuses n > %d", DYN_FLOYD_MAX_N);
    }
    dyn_gk_free(g);
}

static uint32_t rs = 12345;
static uint32_t rnd(uint32_t n)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) % n;
}

static void test_random(void)
{
    int t, bad_sp = 0, bad_topo = 0, bad_reach = 0, bad_mst = 0;
    for (t = 0; t < 400; t++) {
        uint32_t n = 1 + rnd(40), ne = rnd(n * 3 + 1), i, j;
        int directed = (int)rnd(2);
        dyn_graph_t* g = dyn_gk_new(directed, 1);
        double *d1 = malloc(n * sizeof(double)), *d2 = malloc(n * sizeof(double)), *fw = NULL, *h = calloc(n, sizeof(double));
        int* order = malloc(n * sizeof(int));
        int* comp = malloc(n * sizeof(int));
        uint32_t src = rnd(n);
        dyn_gk_ensure(g, n - 1);
        for (i = 0; i < ne; i++)
            dyn_gk_add_edge(g, rnd(n), rnd(n), (double)(rnd(4) ? 1 + rnd(9) : rnd(3)) * 0.5);
        if (dyn_gk_dijkstra(C(g), src, d1, NULL) || dyn_gk_bellman_ford(C(g), src, d2) || dyn_gk_floyd(C(g), &fw))
            bad_sp++;
        else {
            for (i = 0; i < n; i++)
                if (d1[i] != d2[i] || d1[i] != fw[src * n + i])
                    bad_sp++;
            for (j = 0; j < 3; j++) {
                uint32_t dst = rnd(n);
                double ad = -1;
                int* path = NULL;
                uint32_t plen = 0, k;
                double sum = 0;
                if (dyn_gk_astar(C(g), src, dst, h, &ad, &path, &plen) || ad != d1[dst] || (isinf(ad) ? plen != 0 : (plen == 0 || path[0] != (int)src || path[plen - 1] != (int)dst)))
                    bad_sp++;
                for (k = 1; k < plen; k++) {
                    const dyn_gk_csr_t* c = C(g);
                    double best = INFINITY;
                    uint32_t e;
                    for (e = c->off[path[k - 1]]; e < c->off[path[k - 1] + 1]; e++)
                        if (c->tgt[e] == (uint32_t)path[k] && c->w[e] < best)
                            best = c->w[e];
                    sum += best;
                }
                if (plen && sum != ad)
                    bad_sp++;
                free(path);
            }
        }
        if (directed) {
            int64_t no = dyn_gk_topo(C(g), order);
            if (no >= 0) {
                int* pos = xmalloc(n * sizeof(int));
                uint32_t u;
                for (i = 0; i < (uint32_t)no; i++)
                    pos[order[i]] = (int)i;
                const dyn_gk_csr_t* c = C(g);
                for (u = 0; u < n; u++)
                    for (i = c->off[u]; i < c->off[u + 1]; i++)
                        if (pos[u] >= pos[c->tgt[i]])
                            bad_topo++;
                free(pos);
            } else if (no != DYN_GK_E_CYCLE)
                bad_topo++;
        } else {
            int64_t no = dyn_gk_bfs(C(g), src, order);
            uint32_t members = 0, ncomp = 0, ntree = 0;
            dyn_gk_edge_t* tree = NULL;
            double w;
            dyn_gk_components(C(g), comp);
            for (i = 0; i < n; i++) {
                if (comp[i] == comp[src])
                    members++;
                if ((uint32_t)comp[i] + 1 > ncomp)
                    ncomp = (uint32_t)comp[i] + 1;
            }
            if ((uint32_t)no != members)
                bad_reach++;
            for (i = 0; no > 0 && i < (uint32_t)no; i++)
                if (comp[order[i]] != comp[src])
                    bad_reach++;
            if (dyn_gk_mst(C(g), &tree, &ntree, &w) || ntree != n - ncomp)
                bad_mst++;
            free(tree);
        }
        free(d1);
        free(d2);
        free(fw);
        free(h);
        free(order);
        free(comp);
        dyn_gk_free(g);
    }
    CHECK(bad_sp == 0, "dijkstra = bellman-ford = floyd row = A*(h=0), A* paths are real (%d bad)", bad_sp);
    CHECK(bad_topo == 0, "every topological order respects every edge (%d bad)", bad_topo);
    CHECK(bad_reach == 0, "bfs reaches exactly the source's component (%d bad)", bad_reach);
    CHECK(bad_mst == 0, "mst is a spanning forest with n - components edges (%d bad)", bad_mst);
}

static void test_pred_and_dag(void)
{
    enum { S,
        T,
        X,
        Y,
        Z };
    static const edge_t dij[] = { { S, T, 10 }, { S, Y, 5 }, { T, X, 1 }, { T, Y, 2 }, { Y, T, 3 }, { Y, X, 9 }, { Y, Z, 2 }, { X, Z, 4 }, { Z, X, 6 }, { Z, S, 7 } };
    static const edge_t dag[] = { { 0, 1, 5 }, { 0, 2, 3 }, { 1, 2, 2 }, { 1, 3, 6 }, { 2, 3, 7 }, { 2, 4, 4 }, { 2, 5, 2 }, { 3, 4, -1 }, { 3, 5, 1 }, { 4, 5, -2 } };
    static const edge_t neg[] = { { 0, 1, -5 }, { 1, 2, 1 }, { 3, 4, 2 } };
    static const edge_t tie[] = { { 2, 3, 1 }, { 1, 3, 1 }, { 0, 4, 1 }, { 5, 6, 0 } };
    dyn_graph_t* g = build(1, 1, 5, dij, 10);
    double d[7];
    int32_t pred[7], cyc[3] = { 1, 2, 0 };
    int* path = NULL;
    uint32_t len = 0, end = 99;
    int rc;

    rc = dyn_gk_dijkstra(C(g), S, d, pred);
    CHECK(rc == 0 && pred[S] == -1 && pred[Y] == S && pred[T] == Y && pred[X] == T && pred[Z] == Y, "CLRS 24.6 predecessor tree");
    rc = dyn_gk_pred_path(pred, 5, X, &path, &len);
    CHECK(rc == 0 && len == 4 && path[0] == S && path[1] == Y && path[2] == T && path[3] == X, "pred path s,y,t,x (rc=%d len=%u)", rc, len);
    free(path);
    CHECK(dyn_gk_pred_path(cyc, 3, 0, &path, &len) == DYN_GK_E_PRED_CYCLE && !path && len == 0, "a predecessor cycle is reported, not looped on");
    dyn_gk_free(g);

    g = build(1, 1, 6, dag, 10);
    rc = dyn_gk_dag_longest(C(g), d, pred, &end);
    rc = rc ? rc : dyn_gk_pred_path(pred, 6, end, &path, &len);
    CHECK(rc == 0 && end == 5 && d[5] == 15 && len == 5 && path[0] == 0 && path[1] == 1 && path[2] == 2 && path[3] == 3 && path[4] == 5,
        "CLRS 24.5 DAG critical path r,s,t,x,z = 15 (rc=%d end=%u d=%g len=%u)", rc, end, d[5], len);
    free(path);
    dyn_gk_add_edge(g, 5, 0, 1);
    CHECK(dyn_gk_dag_longest(C(g), d, pred, &end) == DYN_GK_E_CYCLE, "a cycle is refused");
    dyn_gk_free(g);

    g = build(1, 1, 5, neg, 3);
    rc = dyn_gk_dag_longest(C(g), d, pred, &end);
    CHECK(rc == 0 && d[1] == 0 && pred[1] == -1 && d[2] == 1 && pred[2] == 1 && end == 4 && d[4] == 2,
        "a negative best prefix restarts the path at the node (networkx dag_longest_path) (d1=%g p1=%d end=%u)", d[1], pred[1], end);
    dyn_gk_free(g);

    g = build(1, 1, 7, tie, 4);
    rc = dyn_gk_dag_longest(C(g), d, pred, &end);
    CHECK(rc == 0 && pred[3] == 1 && end == 3 && pred[6] == 5 && d[6] == 0, "ties go to the smallest predecessor id and the smallest end id (p3=%d end=%u)", pred[3], end);
    dyn_gk_free(g);

    g = dyn_gk_new(1, 0);
    CHECK(dyn_gk_dag_longest(C(g), d, pred, &end) == 0 && end == 0, "an empty graph has an empty critical path");
    dyn_gk_free(g);
}

int main(void)
{
    test_clrs();
    test_pred_and_dag();
    test_walks();
    test_random();
    ring_free();
    printf("test_graphk_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
