#ifndef DYN_GRAPHK_GRAPHK_H
#define DYN_GRAPHK_GRAPHK_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-cdefs.h"

#define DYN_GRAPH_MAX_NODES (1u << 26)
#define DYN_FLOYD_MAX_N 1024
#define DYN_MST_RADIX_MIN 64
#define DYN_GRAPH_MAX_TYPES 32u
#define DYN_GRAPH_ALL_TYPES 0xFFFFFFFFu

typedef struct {
    int to;
    uint8_t t;
    double w;
} dyn_edge_t;

_Static_assert(sizeof(dyn_edge_t) == 16, "dyn_edge_t must stay 16 bytes");

typedef struct {
    dyn_edge_t* e;
    uint32_t n, cap;
} dyn_adj_t;

typedef struct {
    dyn_adj_t* adj;
    uint32_t n;
    uint32_t cap;
    uint64_t m;
    uint64_t typed;
    int directed;
    int weighted;
} dyn_graph_t;

typedef struct {
    double key;
    int node;
} dyn_hp_item;

_Static_assert(sizeof(dyn_hp_item) <= 16, "dyn_hp_item grew past 16 bytes");

typedef struct {
    int u, v;
    double w;
} dyn_gk_edge_t;

enum {
    DYN_GK_OK = 0,
    DYN_GK_E_OOM = -1,
    DYN_GK_E_NEG_WEIGHT = -2,
    DYN_GK_E_NEG_CYCLE = -3,
    DYN_GK_E_CYCLE = -4,
    DYN_GK_E_TOO_LARGE = -5,
    DYN_GK_E_MUTATED = -6,
    DYN_GK_E_CALLBACK = -7,
    DYN_GK_E_BUDGET = -8,
    DYN_GK_E_PRED_CYCLE = -9,
    DYN_GK_E_CANCELLED = -10,
    DYN_GK_E_NO_CONVERGE = -11,
    DYN_GK_E_NOT_BIPARTITE = -12,
    DYN_GK_E_BAD_INPUT = -13
};

typedef struct {
    uint32_t n;
    uint32_t m;
    uint32_t* off;
    uint32_t* tgt;
    double* w;
} dyn_gk_csr_t;

typedef int (*dyn_gk_stop_fn)(void* ud);

typedef struct {
    dyn_gk_stop_fn stop;
    void* ud;
} dyn_gk_ctl_t;

enum {
    DYN_GK_LINK_JACCARD = 0,
    DYN_GK_LINK_ADAMIC_ADAR,
    DYN_GK_LINK_RESOURCE,
    DYN_GK_LINK_COMMON,
    DYN_GK_LINK_PREFERENTIAL
};

typedef struct {
    double alpha;
    double tol;
    uint32_t max_iter;
    const double* pers;
    uint32_t iters;
} dyn_gk_pr_opts_t;

typedef struct {
    double resolution;
    double threshold;
    uint64_t seed;
    uint32_t levels;
    double q;
} dyn_gk_louvain_opts_t;

typedef int (*dyn_gk_visit_fn)(void* ud, int node);

dyn_graph_t* dyn_gk_new(int directed, int weighted);
void dyn_gk_free(void* native);
int dyn_gk_ensure(dyn_graph_t* g, uint32_t id);
int dyn_gk_add_edge(dyn_graph_t* g, uint32_t u, uint32_t v, double w);
int dyn_gk_add_edge_typed(dyn_graph_t* g, uint32_t u, uint32_t v, double w, uint8_t t);
int dyn_gk_csr_of_graph_mask(const dyn_graph_t* g, int with_w, uint32_t mask, dyn_gk_csr_t* out);
int64_t dyn_gk_bfs_mask(const dyn_graph_t* g, uint32_t src, uint32_t mask, int* order);
int64_t dyn_gk_dfs_mask(const dyn_graph_t* g, uint32_t src, uint32_t mask, int* order);
int64_t dyn_gk_khop(const dyn_graph_t* g, uint32_t src, uint32_t k, uint32_t mask, int32_t* order, int32_t* dist);
int64_t dyn_gk_path_ends(const dyn_graph_t* g, const uint32_t* src, uint32_t nsrc, const uint32_t* masks, uint32_t hops, int32_t* out);
int dyn_gk_paths(const dyn_graph_t* g, const uint32_t* src, uint32_t nsrc, const uint32_t* masks, uint32_t hops, int32_t* nodes, uint8_t* types, uint64_t cap, uint64_t budget, uint64_t* total);
int dyn_gk_csr_of_graph_tw(const dyn_graph_t* g, uint32_t mask, const double* tw, dyn_gk_csr_t* out);
int dyn_gk_has_edge(const dyn_graph_t* g, uint32_t u, uint32_t v);
int dyn_gk_has_negative(const dyn_graph_t* g);
int dyn_gk_export_csr(const dyn_graph_t* g, int32_t** off, int32_t** ed, uint64_t* total);

int64_t dyn_gk_bfs(const dyn_gk_csr_t* g, uint32_t src, int* order);
int64_t dyn_gk_dfs(const dyn_gk_csr_t* g, uint32_t src, int* order);
int64_t dyn_gk_bfs_adj(const dyn_graph_t* g, uint32_t src, int* order);
int64_t dyn_gk_dfs_adj(const dyn_graph_t* g, uint32_t src, int* order);
int64_t dyn_gk_topo(const dyn_gk_csr_t* g, int* order);
int dyn_gk_bfs_visit(const dyn_gk_csr_t* g, uint32_t src, dyn_gk_visit_fn fn, void* ud, uint32_t* visited);
int dyn_gk_dfs_visit(const dyn_gk_csr_t* g, uint32_t src, dyn_gk_visit_fn fn, void* ud, uint32_t* visited);
int dyn_gk_topo_visit(const dyn_gk_csr_t* g, dyn_gk_visit_fn fn, void* ud, uint32_t* visited);
int dyn_gk_components(const dyn_gk_csr_t* g, int* comp);

int dyn_gk_dijkstra(const dyn_gk_csr_t* g, uint32_t src, double* dist, int32_t* pred);
int dyn_gk_bellman_ford(const dyn_gk_csr_t* g, uint32_t src, double* dist);
int dyn_gk_floyd(const dyn_gk_csr_t* g, double** out);
int dyn_gk_astar(const dyn_gk_csr_t* g, uint32_t src, uint32_t dst, const double* h, double* dist,
    int** path, uint32_t* plen);
int dyn_gk_mst(const dyn_gk_csr_t* g, dyn_gk_edge_t** out, uint32_t* nout, double* total);
int dyn_gk_pred_path(const int32_t* pred, uint32_t n, uint32_t dst, int** path, uint32_t* plen);
int dyn_gk_dag_longest(const dyn_gk_csr_t* g, double* dist, int32_t* pred, uint32_t* end);

int dyn_gk_csr_alloc(dyn_gk_csr_t* c, uint32_t n, uint64_t m, int with_w);
void dyn_gk_csr_free(dyn_gk_csr_t* c);
int dyn_gk_csr_of_graph(const dyn_graph_t* g, int with_w, dyn_gk_csr_t* out);
int dyn_gk_csr_validate(const dyn_gk_csr_t* c);
int dyn_gk_csr_transpose(const dyn_gk_csr_t* in, int with_w, dyn_gk_csr_t* out);
int dyn_gk_csr_sym(const dyn_gk_csr_t* in, int keep_loops, int with_w, dyn_gk_csr_t* out);
int dyn_gk_csr_dedup(const dyn_gk_csr_t* in, int with_w, dyn_gk_csr_t* out);
int dyn_gk_has_negative_w(const dyn_gk_csr_t* c, int allow_zero);

int dyn_gk_pagerank(const dyn_gk_csr_t* g, dyn_gk_pr_opts_t* o, double* x, const dyn_gk_ctl_t* ctl);
int dyn_gk_topic_pr(const dyn_gk_csr_t* g, uint32_t nt, const double* p, double alpha, double tol, uint32_t max_iter, double* out, uint32_t* iters, uint32_t* bad_topic, const dyn_gk_ctl_t* ctl);
int dyn_gk_lpa(const dyn_gk_csr_t* a, uint64_t seed, uint32_t max_rounds, int32_t* label, uint32_t* ncomm);
int dyn_gk_louvain(const dyn_gk_csr_t* a, dyn_gk_louvain_opts_t* o, int32_t* comm, uint32_t* ncomm, const dyn_gk_ctl_t* ctl);
int dyn_gk_modularity(const dyn_gk_csr_t* g, int directed, const int32_t* comm, double resolution, double* q);
int dyn_gk_core_numbers(const dyn_gk_csr_t* s, int32_t* core);
int dyn_gk_triangles(const dyn_gk_csr_t* s, double* per, double* total, const dyn_gk_ctl_t* ctl);
void dyn_gk_clustering(const dyn_gk_csr_t* s, const double* tri, double* local, double* avg, double* trans);
int dyn_gk_betweenness(const dyn_gk_csr_t* g, const uint32_t* src, uint32_t k, int endpoints, double* bc, const dyn_gk_ctl_t* ctl);
void dyn_gk_betweenness_rescale(double* bc, uint32_t n, int normalized, int directed, int endpoints, const uint32_t* src, uint32_t k, int sampled);
int dyn_gk_closeness(const dyn_gk_csr_t* rev, int wf_improved, double* out, const dyn_gk_ctl_t* ctl);
int dyn_gk_scc(const dyn_gk_csr_t* g, int32_t* comp, uint32_t* ncomp);
int dyn_gk_wcc(const dyn_gk_csr_t* g, int32_t* comp, uint32_t* ncomp);
int dyn_gk_topo_levels(const dyn_gk_csr_t* g, int32_t* level, uint32_t* nlevels);
int dyn_gk_maxflow(const dyn_gk_csr_t* g, uint32_t s, uint32_t t, double* value, double* flow, uint8_t* side, const dyn_gk_ctl_t* ctl);
int dyn_gk_bipartition(const dyn_gk_csr_t* s, uint8_t* left);
int dyn_gk_matching(const dyn_gk_csr_t* s, const uint8_t* left, int32_t* mate, uint32_t* size);
int dyn_gk_link_scores(const dyn_gk_csr_t* s, const uint32_t* pairs, uint32_t np, int method, double* out);

#endif
