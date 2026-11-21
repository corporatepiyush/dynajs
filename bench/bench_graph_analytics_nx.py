#!/usr/bin/env python3
"""networkx timings for bench/bench_graph_analytics.js on the SAME edge list (written by that script's 4th argument).

    ./dynajs bench/bench_graph_analytics.js 20000 16 5 g.edges
    python3 bench/bench_graph_analytics_nx.py g.edges 3
"""
import sys
import time

import networkx as nx

G = nx.read_edgelist(sys.argv[1], nodetype=int, create_using=nx.MultiGraph)
S = nx.Graph(G)
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3


def best(f):
    b = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        f()
        b = min(b, time.perf_counter() - t)
    return b * 1000


rows = [
    ("pageRank (tol 1e-6)", lambda: nx.pagerank(G)),
    ("triangles", lambda: nx.triangles(S)),
    ("clustering", lambda: nx.clustering(S)),
    ("coreNumbers", lambda: nx.core_number(S)),
    ("louvain", lambda: nx.community.louvain_communities(S, seed=1)),
    ("labelPropagation", lambda: list(nx.community.asyn_lpa_communities(S, seed=1))),
    ("betweenness (64 sources)", lambda: nx.betweenness_centrality(S, k=64, seed=1)),
    ("weaklyConnectedComponents", lambda: list(nx.connected_components(S))),
]
print("networkx %s: nodes %d, edges %d, best of %d" % (nx.__version__, G.number_of_nodes(), G.number_of_edges(), runs))
for name, f in rows:
    print("%-30s%10.2f ms" % (name, best(f)))
