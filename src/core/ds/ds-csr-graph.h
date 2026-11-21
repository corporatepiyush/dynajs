#ifndef DS_CSR_GRAPH_H
#define DS_CSR_GRAPH_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_csr dyn_csr_t;

typedef struct {
    uint32_t src, dst;
    double w;
} dyn_csr_edge;

dyn_csr_t* dyn_csr_build(uint32_t nodes, const dyn_csr_edge* edges, size_t nedges, int directed);

void dyn_csr_free(dyn_csr_t* g);

uint32_t dyn_csr_nodes(const dyn_csr_t* g);

size_t dyn_csr_edges(const dyn_csr_t* g);

uint32_t dyn_csr_degree(const dyn_csr_t* g, uint32_t v);

const uint32_t* dyn_csr_neighbors(const dyn_csr_t* g, uint32_t v, const double** w, uint32_t* count);

int dyn_csr_bfs(const dyn_csr_t* g, uint32_t src, int32_t* dist);

int dyn_csr_dijkstra(const dyn_csr_t* g, uint32_t src, double* dist, int32_t* pred);

int dyn_csr_pagerank(const dyn_csr_t* g, double damping, uint32_t iters, double tol, double* rank);

uint32_t dyn_csr_components(const dyn_csr_t* g, uint32_t* comp);

dyn_csr_t* dyn_csr_alloc(uint32_t nodes, size_t arcs, uint32_t** row, uint32_t** col, double** w);

#ifdef __cplusplus
}
#endif

#endif
