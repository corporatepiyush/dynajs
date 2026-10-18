// Graph analytics — PageRank, communities, triangles, shortest paths and spanning trees over a social graph, with
// the iterative kernels expressed as vector operations and every result checked by an independent method.
//
// WHAT IT IS
//   The batch jobs a social or payments company runs nightly over its edge list:
//
//   load        a planted-partition graph (dense communities, sparse bridges) into a native Graph; CSR export gives
//               offset/edge Int32Arrays for the kernels below
//   PageRank    power iteration where each sweep is one gather (edge <- source share) and one scatterAdd
//               (target += edge) over the CSR arrays, teleport and dangling mass
//               added with simd addScalar, convergence measured with simd distL1; the result must be a fixed point
//               of the PageRank equation and sum to 1
//   communities label propagation, scored against the planted partition, and connected components by both
//               the native traversal and a UnionFind over the edge list
//   triangles   per-vertex neighbour sets as RoaringBitmaps; triangles = sum over edges of |N(u) & N(v)| / 3 via
//               native andCardinality; checked by brute force on a subgraph; clustering coefficient per community
//   paths       Dijkstra on a weighted copy agrees with Bellman-Ford and with a BFS on unit weights; the MST
//               weight from Graph.mst equals Kruskal over a sorted edge list with UnionFind
//   influence   top-k PageRank vertices via topkIndices, and how many communities they span
//
// MODULES  dyna:structures (Graph, RoaringBitmap, UnionFind, Multiset) · dyna:simd (addScalar, scale, distL1, sum,
//          topkIndices, gather, scatterAdd) · dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/data/graph-analytics.js

import { Graph, RoaringBitmap, UnionFind, Multiset } from "dyna:structures";
import { addScalar, scale, distL1, sum, topkIndices, gather, scatterAdd } from "dyna:simd";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1998);
const COMMUNITIES = 12,
    PER = 250,
    N = COMMUNITIES * PER,
    P_IN = 0.08,
    P_OUT = 0.0003;

// =============================================================================
// Planted-partition graph
// =============================================================================
const g = new Graph({ directed: false });
for (let i = 0; i < N; i++) g.addNode();
const edges = [],
    community = Int32Array.from({ length: N }, (_, i) => Math.floor(i / PER));
for (let u = 0; u < N; u++)
    for (let v = u + 1; v < N; v++) {
        const p = community[u] === community[v] ? P_IN : P_OUT;
        if (rng.nextFloat() < p) {
            g.addEdge(u, v);
            edges.push([u, v]);
        }
    }
const isolated = N - 1;
const { offsets, edges: adj } = g.exportCSR();
const degree = Int32Array.from({ length: N }, (_, i) => offsets[i + 1] - offsets[i]);
const srcOf = new Int32Array(adj.length);
for (let u = 0; u < N; u++) srcOf.fill(u, offsets[u], offsets[u + 1]);
results["CSR holds both directions of every undirected edge"] = adj.length === 2 * edges.length;

// =============================================================================
// PageRank
// =============================================================================
function pagerank(d = 0.85, tol = 1e-7, maxIter = 200) {
    let pr = new Float32Array(N).fill(1 / N);
    const next = new Float32Array(N),
        share = new Float32Array(N),
        contrib = new Float32Array(adj.length);
    let iters = 0,
        delta = 1;
    while (delta > tol && iters < maxIter) {
        let dangling = 0;
        for (let u = 0; u < N; u++) {
            if (degree[u]) share[u] = pr[u] / degree[u];
            else {
                share[u] = 0;
                dangling += pr[u];
            }
        }
        // Every edge pulls its source's share (gather) and pushes it to its target (scatterAdd), in CSR order:
        // the same additions in the same order as the per-vertex loop, done in two native calls.
        next.fill(0);
        gather(contrib, share, srcOf);
        scatterAdd(next, adj, contrib);
        scale(next, d);
        addScalar(next, (1 - d) / N + (d * dangling) / N);
        delta = distL1(next, pr);
        pr.set(next);
        iters++;
    }
    return { pr, iters };
}
const t0 = performance.now();
const { pr, iters } = pagerank();
const prMs = performance.now() - t0;
results["PageRank sums to 1"] = Math.abs(sum(pr) - 1) < 1e-4;
results["PageRank is a fixed point of its equation"] = (() => {
    let worst = 0;
    for (let v = 0; v < N; v += 7) {
        let incoming = 0;
        for (let k = offsets[v]; k < offsets[v + 1]; k++) incoming += pr[adj[k]] / degree[adj[k]];
        const dangling = degree.reduce((s, dg, u) => (dg ? s : s + pr[u]), 0);
        worst = Math.max(worst, Math.abs(pr[v] - (0.15 / N + 0.85 * (incoming + dangling / N))) / pr[v]);
    }
    return worst < 1e-3;
})();
results["PageRank correlates with degree on an undirected graph"] = (() => {
    const top = Array.from(topkIndices(pr, 20));
    const meanDeg = degree.reduce((a, b) => a + b, 0) / N;
    return top.every((v) => degree[v] > meanDeg);
})();

// =============================================================================
// Components and communities
// =============================================================================
{
    const native = g.connectedComponents();
    const uf = new UnionFind(N);
    for (const [u, v] of edges) uf.union(u, v);
    let agree = true;
    for (const [u, v] of [
        [0, N - 2],
        [5, 777],
        [isolated, 0],
        [100, 2900],
    ])
        if ((native[u] === native[v]) !== uf.connected(u, v)) agree = false;
    const compSizes = new Multiset();
    for (let v = 0; v < N; v++) compSizes.add(String(uf.find(v)));
    results["native components agree with UnionFind"] = agree && new Set(native).size === compSizes.elementSet().length;
}
function labelPropagation(rounds = 15) {
    const label = Int32Array.from({ length: N }, (_, i) => i);
    const order = Int32Array.from({ length: N }, (_, i) => i);
    for (let r = 0; r < rounds; r++) {
        rng.shuffle(order);
        let changed = 0;
        for (const u of order) {
            if (!degree[u]) continue;
            const votes = new Map();
            let best = label[u],
                bestVotes = 0,
                ties = 0;
            for (let k = offsets[u]; k < offsets[u + 1]; k++) {
                const l = label[adj[k]];
                votes.set(l, (votes.get(l) ?? 0) + 1);
            }
            for (const [l, c] of votes) {
                if (c > bestVotes) {
                    best = l;
                    bestVotes = c;
                    ties = 1;
                } else if (c === bestVotes && rng.nextBounded(++ties) === 0) best = l;
            }
            if (votes.get(label[u]) === bestVotes) best = label[u];
            if (best !== label[u]) {
                label[u] = best;
                changed++;
            }
        }
        if (!changed) break;
    }
    return label;
}
const labels = labelPropagation();
// Purity: each found community's majority planted community, weighted by size.
const byLabel = new Map();
for (let v = 0; v < N; v++) {
    if (!degree[v]) continue;
    const m = byLabel.get(labels[v]) ?? byLabel.set(labels[v], new Multiset()).get(labels[v]);
    m.add(String(community[v]));
}
let pure = 0,
    covered = 0;
for (const m of byLabel.values()) {
    const es = m.entrySet();
    pure += Math.max(...es.map(([, c]) => c));
    covered += es.reduce((s, [, c]) => s + c, 0);
}
const bigLabels = [...byLabel.values()].filter((m) => m.entrySet().reduce((s, [, c]) => s + c, 0) > PER / 2).length;
results["label propagation recovers the planted communities (purity > 0.95)"] = pure / covered > 0.95;
results["about twelve large communities found"] = bigLabels >= 10 && bigLabels <= 14;

// =============================================================================
// Triangles via bitmap intersections
// =============================================================================
const nbr = Array.from({ length: N }, (_, u) => new RoaringBitmap(adj.subarray(offsets[u], offsets[u + 1])));
let triangles = 0;
const triPer = new Float64Array(N);
for (const [u, v] of edges) {
    const c = nbr[u].andCardinality(nbr[v]);
    triangles += c;
    triPer[u] += c;
    triPer[v] += c;
}
triangles /= 3;
for (let v = 0; v < N; v++) triPer[v] /= 2;
{
    let brute = 0;
    const S = 400;
    for (let a = 0; a < S; a++)
        for (let b = a + 1; b < S; b++) {
            if (!g.hasEdge(a, b)) continue;
            for (let c = b + 1; c < S; c++) if (g.hasEdge(a, c) && g.hasEdge(b, c)) brute++;
        }
    let viaBitmaps = 0;
    const sub = new RoaringBitmap();
    sub.addRange(0, S);
    for (const [u, v] of edges) if (u < S && v < S) viaBitmaps += nbr[u].clone().and(nbr[v]).and(sub).toArray().length;
    results["bitmap triangle count equals brute force on a 400-vertex subgraph"] = brute === viaBitmaps / 3;
}
const clustering = Array.from({ length: COMMUNITIES }, (_, c) => {
    let s = 0,
        n = 0;
    for (let v = c * PER; v < (c + 1) * PER; v++)
        if (degree[v] > 1) {
            s += triPer[v] / ((degree[v] * (degree[v] - 1)) / 2);
            n++;
        }
    return s / n;
});
results["local clustering is near P_IN inside communities"] = clustering.every(
    (cc) => cc > P_IN * 0.6 && cc < P_IN * 1.6,
);

// =============================================================================
// Shortest paths and spanning trees
// =============================================================================
{
    const wg = new Graph({ directed: false, weighted: true });
    for (let i = 0; i < N; i++) wg.addNode();
    const wedges = edges.map(([u, v]) => [u, v, 1 + ((u * 31 + v * 17) % 9)]);
    for (const [u, v, w] of wedges) wg.addEdge(u, v, w);
    const dj = wg.dijkstra(0),
        bf = wg.bellmanFord(0);
    let pathsAgree = true;
    for (let v = 0; v < N; v += 13) if (dj[v] !== bf[v]) pathsAgree = false;
    results["Dijkstra agrees with Bellman-Ford"] = pathsAgree;
    const unit = g.dijkstra(0),
        order = g.bfs(0),
        hop = new Int32Array(N).fill(-1);
    hop[0] = 0;
    for (const u of order)
        for (let k = offsets[u]; k < offsets[u + 1]; k++) if (hop[adj[k]] < 0) hop[adj[k]] = hop[u] + 1;
    let unitAgree = true;
    for (let v = 0; v < N; v++) if (hop[v] >= 0 && unit[v] !== hop[v]) unitAgree = false;
    results["unit-weight Dijkstra equals BFS hop counts"] = unitAgree;
    const mst = wg.mst();
    const uf = new UnionFind(N);
    let kruskal = 0;
    for (const [u, v, w] of wedges.slice().sort((a, b) => a[2] - b[2])) if (uf.union(u, v)) kruskal += w;
    results["native MST weight equals Kruskal"] = mst.weight === kruskal;
    const reachable = dj.filter((d) => Number.isFinite(d) && d >= 0).length;
    print(
        `paths: ${reachable.format()} vertices reachable from 0, eccentricity ${Math.max(...hop)} hops; MST weight ${mst.weight.format()} over ${mst.edges.length.format()} edges`,
    );
}

// =============================================================================
// Influence summary
// =============================================================================
const top = Array.from(topkIndices(pr, 10)).sort((a, b) => pr[b] - pr[a]);
results["top influencers span several communities"] = new Set(top.map((v) => community[v])).size >= 3;
print(
    Table(
        top.slice(0, 6).map((v) => [v, (pr[v] * N).toFixed(2), degree[v], community[v], triPer[v] | 0]),
        { head: ["vertex", "PR x N", "degree", "community", "triangles"] },
    ),
);
print(
    `PageRank ${iters} iterations in ${prMs.toFixed(0)} ms; ${edges.length.format()} edges, ${triangles.format()} triangles; LPA purity ${(pure / covered).toFixed(3)}`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
