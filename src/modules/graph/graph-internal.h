#ifndef GRAPH_INTERNAL_H
#define GRAPH_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-ds.h"
#include "core/dyn-graphk.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

#include "dyna-serialize.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

extern JSClassID dyn_graph_class_id;

static inline dyn_graph_t* graph_of(JSContext* ctx, JSValueConst t)
{
    return (dyn_graph_t*)dyn_plain_get(ctx, t, dyn_graph_class_id);
}

JSValue dyn_graph_add_edge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_neighbors(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_has_edge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_edge_count(JSContext* ctx, JSValueConst this_val);

JSValue dyn_graph_bfs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_dfs(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_graph_i32_class_id;

JSValue dyn_graph_neighbors_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_dijkstra(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_bellman_ford(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_topo_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_components(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_floyd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_graph_astar(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue dyn_graph_shortest_path(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue dyn_graph_shortest_path_tree(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue dyn_graph_critical_path(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
int dyn_graph_mask_arg(JSContext* ctx, JSValueConst v, const char* op, uint32_t* mask);
int dyn_graph_type_arg(JSContext* ctx, JSValueConst v, const char* op, uint8_t* t);
int dyn_graph_types_opt(JSContext* ctx, JSValueConst opts, const char* op, uint32_t* mask, int* has);
int dyn_graph_csr_for(JSContext* ctx, const dyn_graph_t* g, int with_w, int has, uint32_t mask, dyn_gk_csr_t* c, const char* op);
JSValue dyn_graph_types_array(JSContext* ctx, const dyn_graph_t* g, size_t total);
JSValue dyn_graph_neighborhood(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue dyn_graph_path_endpoints(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue dyn_graph_paths_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue dyn_graph_from_edges(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

enum {
    GX_PAGERANK = 0,
    GX_LPA,
    GX_LOUVAIN,
    GX_MODULARITY,
    GX_CORES,
    GX_TRIANGLES,
    GX_CLUSTERING,
    GX_BETWEENNESS,
    GX_CLOSENESS,
    GX_SCC,
    GX_WCC,
    GX_LEVELS,
    GX_MAXFLOW,
    GX_MATCHING,
    GX_LINK,
    GX_TOPIC,
    GX_COUNT
};

JSValue dyn_graph_algo(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

int dyn_graph_algo_register(JSContext* ctx, JSModuleDef* m);

#endif

#endif
