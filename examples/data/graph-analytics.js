// Graph analytics — PageRank, communities, k-cores, triangles, betweenness, shortest paths and spanning trees over a
// social graph, computed by the native graph kernels of dyna:structures and every result checked by an independent method.
//
// WHAT IT IS
//   The batch jobs a social or payments company runs nightly over its edge list:
//
//   load        a planted-partition graph (dense communities, sparse bridges) into a native Graph; exportCSR gives the
//               offset/edge Int32Arrays the independent checks walk
//   PageRank    g.pageRank() (pull power iteration in C, multi-threaded); the result must be a fixed point of the PageRank
//               equation evaluated here in JS, sum to 1, and equal graph.pageRank over the exported CSR
//   communities g.labelPropagation() and g.louvain(), scored against the planted partition; Louvain's reported modularity
//               must equal the definition summed here edge by edge; connected components by the native union-find
//               and by a UnionFind over the edge list
//   triangles   g.triangles() / g.clustering() (degree-ordered merge intersection in C) against an independent count:
//               per-vertex neighbour sets as RoaringBitmaps, sum over edges of |N(u) & N(v)| / 3, itself checked by brute
//               force on a 400-vertex subgraph; clustering coefficient per community
//   paths       Dijkstra on a weighted copy agrees with Bellman-Ford and with a BFS on unit weights; the MST weight from
//               Graph.mst equals Kruskal over a sorted edge list with UnionFind
//   cores       g.coreNumbers() (Batagelj–Zaversnik in C), equal for every vertex to the definition run naively here
//               (delete every vertex of degree < k until none is left, k = 1, 2, ...)
//   brokers     g.betweenness(): exact on a 90-vertex two-community subgraph, where it must equal the definition (sum of
//               sigma_st(v)/sigma_st over all pairs, counted here by BFS) and crown the single bridge; sampled from 48
//               sources on the whole graph, where its top vertices must be the endpoints of inter-community edges
//   influence   top-k PageRank vertices via topkIndices, and how many communities they span
//
// MODULES  dyna:structures (Graph, graph, RoaringBitmap, UnionFind, Multiset) · dyna:simd (topkIndices, cumsum) ·
//          dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/data/graph-analytics.js

import { RoaringBitmap, UnionFind, Multiset } from "dyna:structures";
import { Graph, graph } from "dyna:graph";
import { topkIndices, cumsum } from "dyna:simd";
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
results["CSR holds both directions of every undirected edge"] = adj.length === 2 * edges.length;

// =============================================================================
// PageRank
// =============================================================================
const t0 = performance.now();
const pr = await g.pageRank({ damping: 0.85, tolerance: 1e-10, maxIterations: 200 });
const prMs = performance.now() - t0;
// the same kernel over plain typed arrays: the CSR a file or another process would hand over
const prCsr = await graph.pageRank(g.exportCSR(), { damping: 0.85, tolerance: 1e-10, maxIterations: 200 });
results["graph.pageRank over the exported CSR equals Graph#pageRank bit for bit"] = prCsr.every((x, i) => x === pr[i]);
results["PageRank sums to 1"] = Math.abs(pr.reduce((a, b) => a + b, 0) - 1) < 1e-9;
results["PageRank is a fixed point of its equation"] = (() => {
    let worst = 0;
    for (let v = 0; v < N; v += 7) {
        let incoming = 0;
        for (let k = offsets[v]; k < offsets[v + 1]; k++) incoming += pr[adj[k]] / degree[adj[k]];
        const dangling = degree.reduce((s, dg, u) => (dg ? s : s + pr[u]), 0);
        worst = Math.max(worst, Math.abs(pr[v] - (0.15 / N + 0.85 * (incoming + dangling / N))) / pr[v]);
    }
    return worst < 1e-6;
})();
results["PageRank correlates with degree on an undirected graph"] = (() => {
    const top = Array.from(topkIndices(Float32Array.from(pr), 20));
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
const labels = await g.labelPropagation({ seed: 1998 });
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
const lv = await g.louvain({ seed: 1998 });
{
    // the definition: Q = sum_c L_c/m - (d_c/2m)^2, summed here edge by edge over the planted graph
    const m = edges.length,
        L = new Float64Array(N),
        D = new Float64Array(N);
    for (const [u, v] of edges) {
        if (lv.communities[u] === lv.communities[v]) L[lv.communities[u]]++;
        D[lv.communities[u]]++;
        D[lv.communities[v]]++;
    }
    let q = 0;
    for (let c = 0; c < N; c++) q += L[c] / m - (D[c] / (2 * m)) ** 2;
    let agree = 0;
    for (const [u, v] of edges)
        if ((lv.communities[u] === lv.communities[v]) === (community[u] === community[v])) agree++;
    results[
        `Louvain modularity ${lv.modularity.toFixed(4)} equals the definition and the planted split (${lv.count} communities)`
    ] = Math.abs(q - lv.modularity) < 1e-9 && agree / m > 0.97;
}

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
const tNative = performance.now(),
    nativeTri = await g.triangles(),
    triMs = performance.now() - tNative,
    clus = await g.clustering();
results["native triangles equal the bitmap intersections, total and per vertex"] =
    nativeTri.total === triangles && nativeTri.perVertex.every((t, v) => t === triPer[v]);
results["native local clustering equals triangles / (d choose 2)"] = clus.local.every(
    (c, v) => Math.abs(c - (degree[v] > 1 ? triPer[v] / ((degree[v] * (degree[v] - 1)) / 2) : 0)) < 1e-12,
);
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
// k-cores: native Batagelj–Zaversnik peeling, checked against naive repeated peeling
// =============================================================================
const core = await g.coreNumbers();
{
    // the definition: the k-core is what survives deleting every vertex of degree < k until none is left
    const alive = new Uint8Array(N).fill(1),
        d = degree.slice(),
        naive = new Int32Array(N);
    for (let k = 1, left = N; left; k++) {
        let removed = true;
        while (removed) {
            removed = false;
            for (let v = 0; v < N; v++)
                if (alive[v] && d[v] < k) {
                    alive[v] = 0;
                    naive[v] = k - 1;
                    left--;
                    removed = true;
                    for (let e = offsets[v]; e < offsets[v + 1]; e++) d[adj[e]]--;
                }
        }
    }
    const maxCore = Math.max(...core),
        inner = Array.from({ length: N }, (_, v) => v).filter((v) => core[v] === maxCore);
    results[`native core numbers equal naive peeling for all ${N} vertices (degeneracy ${maxCore})`] =
        core.every((c, v) => c === naive[v]) &&
        inner.every((v) => {
            let k = 0;
            for (let e = offsets[v]; e < offsets[v + 1]; e++) if (core[adj[e]] >= maxCore) k++;
            return k >= maxCore;
        });
}

// =============================================================================
// Betweenness: native Brandes, exact on a subgraph (against path counting), sampled on the whole graph
// =============================================================================
{
    // induced subgraph: the first 45 vertices of communities 0 and 1 plus every bridge endpoint among them
    const keep = [...Array.from({ length: 45 }, (_, i) => i), ...Array.from({ length: 45 }, (_, i) => PER + i)],
        local = new Map(keep.map((v, i) => [v, i])),
        n = keep.length,
        lists = keep.map((v) =>
            Array.from(adj.subarray(offsets[v], offsets[v + 1]))
                .filter((u) => local.has(u))
                .map((u) => local.get(u)),
        );
    lists[0].push(n - 1);
    lists[n - 1].push(0); // one planted bridge, so the two halves are connected through exactly one edge
    const off = Int32Array.from([0, ...lists.map((l) => l.length)]);
    cumsum(off);
    const nb = Int32Array.from(lists.flat());
    // native exact betweenness over the subgraph as a plain CSR; raw undirected scores count each unordered pair once,
    // the definition below sums ordered pairs, hence the factor 2
    const exact = (await graph.betweenness({ offsets: off, edges: nb, directed: false }, { normalized: false })).map(
        (x) => 2 * x,
    );
    // the definition: sum over ordered pairs (s, t) of sigma_st(v) / sigma_st, from all-pairs BFS distances and counts
    const D = [],
        SG = [];
    for (let s = 0; s < n; s++) {
        const d = new Int32Array(n).fill(-1),
            sg = new Float64Array(n),
            q = [s];
        d[s] = 0;
        sg[s] = 1;
        for (let h = 0; h < q.length; h++)
            for (let e = off[q[h]]; e < off[q[h] + 1]; e++) {
                const w = nb[e];
                if (d[w] < 0) {
                    d[w] = d[q[h]] + 1;
                    q.push(w);
                }
                if (d[w] === d[q[h]] + 1) sg[w] += sg[q[h]];
            }
        D.push(d);
        SG.push(sg);
    }
    let worst = 0;
    for (let v = 0; v < n; v++) {
        let b = 0;
        for (let s = 0; s < n; s++)
            for (let t = 0; t < n; t++)
                if (s !== v && t !== v && s !== t && D[s][t] > 0 && D[s][v] >= 0 && D[s][v] + D[v][t] === D[s][t])
                    b += (SG[s][v] * SG[v][t]) / SG[s][t];
        worst = Math.max(worst, Math.abs(b - exact[v]) / Math.max(1, b));
    }
    const broker = exact.indexOf(Math.max(...exact));
    results[
        `Brandes betweenness equals path counting on a ${n}-vertex subgraph; the bridge endpoint is the top broker`
    ] = worst < 1e-9 && (broker === 0 || broker === n - 1);
}
// whole graph, 48 sampled sources: the vertices that carry the most shortest paths are the ones holding bridges
const SAMPLES = 48,
    tB = performance.now(),
    bcApprox = await g.betweenness({ samples: SAMPLES, seed: 1998 }),
    bcMs = performance.now() - tB;
// bridges are common here (~0.8 per vertex), so the test is how MANY each broker holds against the population
const interDeg = new Int32Array(N);
for (const [u, v] of edges) if (community[u] !== community[v]) (interDeg[u]++, interDeg[v]++);
const topBrokers = Array.from(topkIndices(Float32Array.from(bcApprox), 30));
const brokerBridges = topBrokers.reduce((a, v) => a + interDeg[v], 0) / topBrokers.length,
    meanBridges = interDeg.reduce((a, b) => a + b, 0) / N;
results[
    `the top-30 sampled-betweenness vertices hold ${brokerBridges.toFixed(1)} inter-community edges each (population ${meanBridges.toFixed(2)})`
] = brokerBridges > 2.5 * meanBridges;

// =============================================================================
// Influence summary
// =============================================================================
const top = Array.from(topkIndices(Float32Array.from(pr), 10)).sort((a, b) => pr[b] - pr[a]);
results["top influencers span several communities"] = new Set(top.map((v) => community[v])).size >= 3;
print(
    Table(
        top.slice(0, 6).map((v) => [v, (pr[v] * N).toFixed(2), degree[v], community[v], triPer[v] | 0]),
        { head: ["vertex", "PR x N", "degree", "community", "triangles"] },
    ),
);
print(
    `PageRank in ${prMs.toFixed(1)} ms; ${edges.length.format()} edges, ${triangles.format()} triangles (native count ${triMs.toFixed(1)} ms); LPA purity ${(pure / covered).toFixed(3)}; Louvain ${lv.count} communities over ${lv.levels} levels`,
);
print(
    `k-cores: degeneracy ${Math.max(...core)}; betweenness from ${SAMPLES} sampled sources in ${bcMs.toFixed(1)} ms (${(bcMs / SAMPLES).toFixed(2)} ms per BFS + back-propagation, all cores)`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
