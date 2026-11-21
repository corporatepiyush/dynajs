#!/usr/bin/env python3
"""Generate tests/corpus/ref/graph_paths_vectors.js: reference answers for Graph#shortestPath,
Graph#shortestPathTree and Graph#criticalPath.

The graphs are NOT stored: tests/graph/test_graph_graph_paths_ref.js rebuilds each one
from the same splitmix64 stream (BigInt in JS), so both sides see identical edge lists, parallel
edges and self loops included. Weights are dyadic rationals (k / 1024, or (k - 1024) / 32 for the
signed DAGs), so every path sum is exact in double and answers compare with ===.

Every expected value comes from networkx (version recorded in the output):
  - dist / path: nx.single_source_dijkstra on a Multi(Di)Graph (parallel edges: the lighter one).
    A path (and the pred entry) is recorded only when it is the unique shortest path, checked with
    nx.all_shortest_paths; otherwise only the distance is (pred -2 = "any shortest predecessor").
  - criticalPath length and path: nx.dag_longest_path_length / nx.dag_longest_path (weight
    "weight", default_weight 1, max over parallel edges); the path is recorded only when it is the
    unique longest path, otherwise null.
  - criticalPath dist[v] (longest path ending at v, never below 0, as dag_longest_path restarts on a
    negative prefix): max(0, -d) over nx.single_source_bellman_ford_path_length from v on the
    reversed graph with negated weights, restricted to v's ancestors.
Nothing here reads an answer from dynajs. Regenerate with the pinned env plus networkx:

    .scratch/nxenv/bin/python tests/ref/gen_graph_paths_ref_vectors.py > tests/corpus/ref/graph_paths_vectors.js
"""
import itertools
import json
import math

import networkx as nx
import sys


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
    n, m, kind = spec["n"], spec["m"], spec["kind"]
    edges = []
    for _ in range(m):
        u = r.next() % n
        v = r.next() % n
        k = r.next() % 2048
        if kind == "dag":
            if u == v:
                continue
            u, v = min(u, v), max(u, v)
            w = (k - 1024) / 32 if spec.get("signed") else k / 1024
        else:
            w = k / 1024
        edges.append((u, v, w))
        if r.next() % 8 == 0:
            edges.append((u, v, w + 0.5))
    return n, edges


def multigraph(n, edges, directed):
    G = nx.MultiDiGraph() if directed else nx.MultiGraph()
    G.add_nodes_from(range(n))
    for u, v, w in edges:
        G.add_edge(u, v, weight=w)
    return G


def num(x):
    return None if math.isinf(x) else x


def sssp(spec):
    n, edges = build(spec)
    G = multigraph(n, edges, spec["directed"])
    src = spec["src"]
    dist, paths = nx.single_source_dijkstra(G, src, weight="weight")
    d, pred, unique = [], [], {}
    for v in range(n):
        if v not in dist:
            d.append(None)
            pred.append(-1)
            continue
        d.append(dist[v])
        many = list(itertools.islice(nx.all_shortest_paths(G, src, v, weight="weight"), 2))
        if len(many) == 1:
            unique[v] = paths[v]
            pred.append(-1 if v == src else paths[v][-2])
        else:
            pred.append(-2)
    queries = []
    for t in spec["dst"]:
        queries.append({"dst": t, "dist": num(d[t]) if d[t] is not None else None, "path": unique.get(t) if d[t] is not None else []})
    return {"dist": d, "pred": pred, "queries": queries}


def longest_unique(G, n, path, length):
    best = [0.0] * n
    ties = False
    for v in nx.topological_sort(G):
        cands = [best[u] + max(e["weight"] for e in G[u][v].values()) for u in G.predecessors(v)]
        top = max(cands) if cands else -math.inf
        if cands and (cands.count(top) > 1 or top == 0):
            ties = ties or v in path
        best[v] = top if top >= 0 else 0.0
    if best.count(length) > 1:
        ties = True
    return not ties


def critical(spec):
    n, edges = build(spec)
    G = multigraph(n, edges, True)
    length = nx.dag_longest_path_length(G, weight="weight", default_weight=1)
    path = nx.dag_longest_path(G, weight="weight", default_weight=1)
    R = nx.MultiDiGraph()
    R.add_nodes_from(range(n))
    for u, v, w in edges:
        R.add_edge(v, u, weight=-w)
    dist = []
    for v in range(n):
        sp = nx.single_source_bellman_ford_path_length(R, v, weight="weight")
        dist.append(max(0.0, max(-x for x in sp.values())))
    return {"length": length, "path": path if longest_unique(G, n, path, length) else None, "dist": dist}


SSSP = [
    {"seed": 11, "kind": "gnm", "n": 12, "m": 30, "directed": True, "src": 0, "dst": [5, 11]},
    {"seed": 12, "kind": "gnm", "n": 40, "m": 90, "directed": True, "src": 3, "dst": [0, 17, 39]},
    {"seed": 13, "kind": "gnm", "n": 40, "m": 60, "directed": False, "src": 0, "dst": [1, 20, 39]},
    {"seed": 14, "kind": "gnm", "n": 120, "m": 360, "directed": True, "src": 7, "dst": [0, 60, 119]},
    {"seed": 15, "kind": "gnm", "n": 120, "m": 200, "directed": False, "src": 119, "dst": [0, 5, 64]},
    {"seed": 16, "kind": "gnm", "n": 300, "m": 700, "directed": True, "src": 0, "dst": [1, 150, 299]},
    {"seed": 17, "kind": "gnm", "n": 30, "m": 20, "directed": True, "src": 2, "dst": [3, 29]},
    {"seed": 18, "kind": "gnm", "n": 1, "m": 3, "directed": True, "src": 0, "dst": [0]},
]
CRIT = [
    {"seed": 21, "kind": "dag", "n": 10, "m": 25},
    {"seed": 22, "kind": "dag", "n": 50, "m": 150},
    {"seed": 23, "kind": "dag", "n": 50, "m": 150, "signed": True},
    {"seed": 24, "kind": "dag", "n": 200, "m": 700},
    {"seed": 25, "kind": "dag", "n": 200, "m": 500, "signed": True},
    {"seed": 26, "kind": "dag", "n": 40, "m": 15, "signed": True},
    {"seed": 27, "kind": "dag", "n": 400, "m": 1600},
]
out = {
    "networkx": pinned("networkx", nx.__version__),
    "sssp": [dict(spec=s, **sssp(s)) for s in SSSP],
    "critical": [dict(spec=s, **critical(s)) for s in CRIT],
}
print("// generated by tests/ref/gen_graph_paths_ref_vectors.py; do not edit")
print("export default " + json.dumps(out, separators=(",", ":"), allow_nan=False) + ";")
