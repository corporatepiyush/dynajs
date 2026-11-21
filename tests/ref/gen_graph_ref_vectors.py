#!/usr/bin/env python3
"""Generate tests/corpus/ref/graph_vectors.js: dyna:structures graph-analytics reference values.

The graphs are NOT stored: tests/graph/test_graph_graph_analytics_ref.js rebuilds
each one from the same splitmix64 stream (integer arithmetic only, BigInt in JS),
so Python and JS see identical edge lists, parallel edges and self loops included.
Only the expected answers are vendored.

Every expected value comes from networkx (version recorded in the header), each
call mapped to the convention dynajs.d.ts states for the dyna:structures method:
  - pageRank: nx.pagerank on a Multi(Di)Graph (parallel weights add up),
    personalized with personalization = {v: 1 if v == 0 else v % 5};
  - betweenness: nx.betweenness_centrality on the simple graph (no self loops,
    parallel edges merged keeping the MINIMUM weight), normalized / raw /
    endpoints, and the sampled estimator from networkx's own helpers
    (_single_source_shortest_path_basic, _accumulate_basic, _rescale) over a
    fixed source list, which is what betweenness({ sources }) computes;
  - closeness: nx.closeness_centrality (incoming distance on digraphs),
    wf_improved true and false, unweighted and weighted;
  - triangles, clustering, average_clustering, transitivity, core_number,
    jaccard / adamic_adar / resource_allocation / preferential_attachment and
    hopcroft_karp_matching: on the underlying undirected simple graph;
  - strongly / weakly connected components (as partitions), topological
    generations, maximum_flow_value (parallel capacities summed, s = 0,
    t = n - 1), modularity of the partition v % 4 (resolution 1 and 0.7).
  - louvain_communities(seed=7): only its modularity is recorded, as the
    quality bar the native Louvain must reach (the partitions depend on each
    implementation's random order).
Nothing here reads an answer from dynajs. Regenerate with the pinned env plus
networkx:

    .scratch/nxenv/bin/python tests/ref/gen_graph_ref_vectors.py > tests/corpus/ref/graph_vectors.js
"""
import math
import sys

import networkx as nx
from networkx.algorithms.centrality.betweenness import (
    _accumulate_basic,
    _rescale,
    _single_source_shortest_path_basic,
)


def pinned(name, have):
    """The version written into the vectors is the pin in tests/ref/requirements.txt, never the
    running one: a runtime version string made the file's sha256 differ between two machines
    that computed the same numbers. A different running version is reported on stderr."""
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    for line in open(os.path.join(here, "requirements.txt")):
        if line.startswith(name + "=="):
            pin = line.strip().split("==", 1)[1]
            if pin != have:
                sys.stderr.write("%s: %s %s is running, the vectors record the pin %s\n" % (os.path.basename(__file__), name, have, pin))
            return pin
    raise SystemExit("tests/ref/requirements.txt pins no " + name)

M64 = (1 << 64) - 1


class SplitMix:
    def __init__(self, seed):
        self.s = seed & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


def build(spec):
    r = SplitMix(spec["seed"])
    kind = spec["kind"]
    edges = []
    weighted = spec.get("weighted", False)

    def wt():
        return 1 + r.next() % 9 if weighted else 1

    if kind == "gnm":
        n = spec["n"]
        for _ in range(spec["m"]):
            u = r.next() % n
            v = r.next() % n
            edges.append((u, v, wt()))
    elif kind == "planted":
        k, s = spec["k"], spec["size"]
        n = k * s
        for u in range(n):
            for v in range(u + 1, n):
                p = spec["pin"] if u // s == v // s else spec["pout"]
                if r.next() % 1000000 < p:
                    edges.append((u, v, wt()))
    elif kind == "dag":
        n = spec["n"]
        for _ in range(spec["m"]):
            u = r.next() % n
            v = r.next() % n
            if u != v:
                edges.append((min(u, v), max(u, v), wt()))
    elif kind == "bip":
        nl, nr = spec["left"], spec["right"]
        n = nl + nr
        for _ in range(spec["m"]):
            u = r.next() % nl
            v = nl + r.next() % nr
            edges.append((u, v, wt()))
    else:
        raise ValueError(kind)
    return n, edges


def multigraph(n, edges, directed):
    G = nx.MultiDiGraph() if directed else nx.MultiGraph()
    G.add_nodes_from(range(n))
    for u, v, w in edges:
        G.add_edge(u, v, weight=w)
    return G


def simple(n, edges, directed, loops=False):
    G = nx.DiGraph() if directed else nx.Graph()
    G.add_nodes_from(range(n))
    for u, v, w in edges:
        if u == v and not loops:
            continue
        if G.has_edge(u, v):
            G[u][v]["weight"] = min(G[u][v]["weight"], w)
        else:
            G.add_edge(u, v, weight=w)
    return G


def summed(n, edges, directed):
    G = nx.DiGraph() if directed else nx.Graph()
    G.add_nodes_from(range(n))
    for u, v, w in edges:
        if u == v:
            continue
        if G.has_edge(u, v):
            G[u][v]["capacity"] += w
        else:
            G.add_edge(u, v, capacity=w)
    return G


def vec(d, n):
    return [float(d[v]) for v in range(n)]


def canon(parts, n):
    lab = [0] * n
    for p in parts:
        m = min(p)
        for v in p:
            lab[v] = m
    return lab


def sampled_bc(G, sources, normalized):
    bc = dict.fromkeys(G, 0.0)
    for s in sources:
        S, P, sigma, _ = _single_source_shortest_path_basic(G, s)
        bc, _ = _accumulate_basic(bc, S, P, sigma, s)
    bc = _rescale(bc, len(G), normalized=normalized, directed=G.is_directed(),
                  endpoints=False, sampled_nodes=sources)
    return vec(bc, len(G))


def case(name, spec, directed, small=True):
    n, edges = build(spec)
    weighted = spec.get("weighted", False)
    out = {"name": name, "spec": spec, "directed": directed, "n": n, "m": len(edges)}
    M = multigraph(n, edges, directed)
    S = simple(n, edges, directed)
    U = simple(n, edges, False)
    res = {}
    res["pagerank"] = vec(nx.pagerank(M, tol=1e-12, max_iter=10000), n)
    res["pagerankPersonal"] = vec(nx.pagerank(M, personalization={v: (1 if v == 0 else v % 5) for v in range(n)}, tol=1e-12, max_iter=10000), n)
    tri = nx.triangles(U)
    res["triangles"] = vec(tri, n)
    res["triangleTotal"] = sum(tri.values()) // 3
    res["clustering"] = vec(nx.clustering(U), n)
    res["averageClustering"] = nx.average_clustering(U) if n else 0.0
    res["transitivity"] = nx.transitivity(U)
    res["coreNumbers"] = vec(nx.core_number(U), n)
    res["wcc"] = canon(nx.weakly_connected_components(S) if directed else nx.connected_components(S), n)
    if directed:
        res["scc"] = canon(nx.strongly_connected_components(S), n)
        if nx.is_directed_acyclic_graph(S):
            lev = [0] * n
            for i, gen in enumerate(nx.topological_generations(S)):
                for v in gen:
                    lev[v] = i
            res["levels"] = lev
    if small:
        res["betweenness"] = vec(nx.betweenness_centrality(S, weight=None), n)
        res["betweennessRaw"] = vec(nx.betweenness_centrality(S, normalized=False, weight=None), n)
        res["betweennessEndpoints"] = vec(nx.betweenness_centrality(S, endpoints=True, weight=None), n)
        src = sorted(set((i * 7 + 3) % n for i in range(max(2, n // 6))))
        res["betweennessSources"] = src
        res["betweennessSampled"] = sampled_bc(S, src, True)
        res["closeness"] = vec(nx.closeness_centrality(S), n)
        res["closenessNoWf"] = vec(nx.closeness_centrality(S, wf_improved=False), n)
        if weighted:
            res["betweennessWeighted"] = vec(nx.betweenness_centrality(S, weight="weight"), n)
            res["closenessWeighted"] = vec(nx.closeness_centrality(S, distance="weight"), n)
        pairs = [((i * 7) % n, (i * 13 + 5) % n) for i in range(20)]
        res["pairs"] = pairs
        res["jaccard"] = [p for _, _, p in nx.jaccard_coefficient(U, pairs)]
        res["adamicAdar"] = [p for _, _, p in nx.adamic_adar_index(U, [(a, b) for a, b in pairs])]
        res["resourceAllocation"] = [p for _, _, p in nx.resource_allocation_index(U, pairs)]
        res["preferentialAttachment"] = [float(p) for _, _, p in nx.preferential_attachment(U, pairs)]
        res["commonNeighbors"] = [float(len(list(nx.common_neighbors(U, a, b)))) for a, b in pairs]
    part = {}
    for v in range(n):
        part.setdefault(v % 4, set()).add(v)
    res["modularity"] = nx.community.modularity(M, part.values(), weight="weight")
    res["modularity07"] = nx.community.modularity(M, part.values(), weight="weight", resolution=0.7)
    if not directed:
        lc = nx.community.louvain_communities(M, weight="weight", seed=7)
        res["louvainModularity"] = nx.community.modularity(M, lc, weight="weight")
    if n >= 2:
        F = summed(n, edges, directed)
        res["maxFlow"] = float(nx.maximum_flow_value(F, 0, n - 1))
    if spec["kind"] == "bip":
        top = set(range(spec["left"]))
        mt = nx.bipartite.hopcroft_karp_matching(U, top_nodes=top)
        res["matching"] = len(mt) // 2
    out["results"] = res
    return out


def js(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        if math.isnan(v):
            return "NaN"
        if math.isinf(v):
            return "Infinity" if v > 0 else "-Infinity"
        return repr(v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(js(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(k + ": " + js(x) for k, x in v.items()) + " }"
    raise TypeError(type(v))


CASES = [
    ("undirected multigraph with self loops", {"kind": "gnm", "n": 60, "m": 150, "seed": 11}, False),
    ("weighted undirected multigraph", {"kind": "gnm", "n": 80, "m": 240, "seed": 12, "weighted": True}, False),
    ("directed multigraph with self loops", {"kind": "gnm", "n": 70, "m": 220, "seed": 13}, True),
    ("weighted digraph", {"kind": "gnm", "n": 60, "m": 200, "seed": 14, "weighted": True}, True),
    ("sparse disconnected undirected", {"kind": "gnm", "n": 60, "m": 40, "seed": 15}, False),
    ("sparse disconnected digraph", {"kind": "gnm", "n": 50, "m": 45, "seed": 16}, True),
    ("planted partition 4x30", {"kind": "planted", "k": 4, "size": 30, "pin": 250000, "pout": 10000, "seed": 17}, False),
    ("weighted planted partition 3x25", {"kind": "planted", "k": 3, "size": 25, "pin": 300000, "pout": 20000, "seed": 18, "weighted": True}, False),
    ("random DAG", {"kind": "dag", "n": 50, "m": 120, "seed": 19}, True),
    ("bipartite 30x25", {"kind": "bip", "left": 30, "right": 25, "m": 80, "seed": 20}, False),
    ("edgeless", {"kind": "gnm", "n": 5, "m": 0, "seed": 21}, False),
    ("single node", {"kind": "gnm", "n": 1, "m": 0, "seed": 22}, True),
]

BIG = [
    ("planted partition 8x250", {"kind": "planted", "k": 8, "size": 250, "pin": 60000, "pout": 300, "seed": 23}, False),
]


def main():
    w = sys.stdout.write
    w("// GENERATED by tests/ref/gen_graph_ref_vectors.py -- do not edit by hand.\n")
    w("// networkx %s; graphs are rebuilt from splitmix64 by the test.\n" % pinned("networkx", nx.__version__))
    w("export const CASES = [\n")
    for name, spec, directed in CASES:
        w("    " + js(case(name, spec, directed)) + ",\n")
    w("];\n")
    w("export const BIG = [\n")
    for name, spec, directed in BIG:
        c = case(name, spec, directed, small=False)
        r = c["results"]
        keep = {k: r[k] for k in ("triangleTotal", "averageClustering", "transitivity", "modularity", "louvainModularity")}
        keep["coreMax"] = max(r["coreNumbers"])
        keep["coreSum"] = sum(r["coreNumbers"])
        keep["pagerank"] = r["pagerank"]
        keep["triangles"] = r["triangles"]
        c["results"] = keep
        w("    " + js(c) + ",\n")
    w("];\n")


if __name__ == "__main__":
    main()
