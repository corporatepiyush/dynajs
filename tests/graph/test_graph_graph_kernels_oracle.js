// covers: dyna:graph.Graph
// Graph kernel rewrites that must not change any result:
// - closeness (unweighted) runs a level-synchronous BFS with branchless discovery (stamp written on every edge, the
//   queue slot always written, the count bumped by the fresh flag) and sums distances per level instead of a dist[] array;
// - the Dijkstra heap (dijkstra, shortestPath, shortestPathTree, aStar) sifts with a hole instead of swaps (same
//   comparisons, same final heap, so the same pop order and predecessors);
// - floydWarshall relaxes a row with a branch-free select through restrict row pointers (row k itself stays scalar).
// betweenness, labelPropagation and louvain are pinned too (their rewrites were measured and reverted: no win).
// Oracles: JS BFS closeness (exact), JS Brandes (1e-9 relative), JS Floyd-Warshall (exact), JS Dijkstra distances
// (exact) with predecessor consistency, and fingerprints of the label/community/predecessor outputs recorded from the
// binary before these changes (the seeded tie-breaking must be untouched).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_kernels_oracle");
import { Graph } from "dyna:graph";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 7;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
function mk(n, directed, weighted, deg, block) {
    const g = new Graph({ directed, weighted }), E = [];
    for (let i = 0; i < n; i++) g.addNode();
    for (let u = 0; u < n; u++)
        for (let k = 0; k < deg; k++) {
            const v = rnd() < 0.8 ? Math.min(n - 1, Math.floor(u / block) * block + ((rnd() * block) | 0)) : (rnd() * n) | 0;
            if (v === u) continue;
            const w = 1 + ((rnd() * 5) | 0);
            g.addEdge(u, v, w);
            E.push([u, v, w]);
        }
    return { g, E, n, directed };
}
function adj(G, rev) {
    const a = Array.from({ length: G.n }, () => new Map());
    for (const [u, v, w] of G.E) {
        const put = (x, y) => { if (x !== y && (!a[x].has(y) || a[x].get(y) > w)) a[x].set(y, w); };
        if (G.directed) { if (rev) put(v, u); else put(u, v); } else { put(u, v); put(v, u); }
    }
    return a;
}
const fp = (a) => { let x = 0; for (const v of a) x = (Math.imul(x, 31) + Math.round(v * 1e6)) | 0; return x; };

seed = 7;
const small = [mk(1, false, false, 1, 1), mk(2, false, false, 1, 2), mk(60, false, false, 2, 10), mk(400, false, false, 3, 40), mk(1500, false, false, 4, 50)];
seed = 11;
const dsmall = [mk(300, true, false, 3, 30)];

await __kitS.section("closeness equals a JS BFS per node", async () => {
    for (const G of small.concat(dsmall)) {
        const a = adj(G, true), n = G.n;
        for (const wf of [true, false]) {
            const got = await G.g.closeness({ wfImproved: wf });
            let bad = 0;
            for (let u = 0; u < n; u++) {
                const dist = new Int32Array(n).fill(-1), q = [u];
                dist[u] = 0;
                let tot = 0;
                for (let h = 0; h < q.length; h++) for (const x of a[q[h]].keys()) if (dist[x] < 0) { dist[x] = dist[q[h]] + 1; tot += dist[x]; q.push(x); }
                const reach = q.length - 1;
                let cc = 0;
                if (tot > 0 && n > 1) { cc = reach / tot; if (wf) cc *= reach / (n - 1); }
                if (got[u] !== cc) bad++;
            }
            eq(bad, 0, "closeness n=" + n + " directed=" + G.directed + " wf=" + wf);
        }
    }
});

await __kitS.section("betweenness equals a JS Brandes", async () => {
    for (const G of small.concat(dsmall)) {
        const a = adj(G, false), n = G.n, bc = new Float64Array(n);
        for (let s = 0; s < n; s++) {
            const S = [], P = Array.from({ length: n }, () => []), sigma = new Float64Array(n), d = new Int32Array(n).fill(-1), delta = new Float64Array(n);
            sigma[s] = 1; d[s] = 0;
            const q = [s];
            for (let h = 0; h < q.length; h++) { const v = q[h]; S.push(v); for (const w of a[v].keys()) { if (d[w] < 0) { q.push(w); d[w] = d[v] + 1; } if (d[w] === d[v] + 1) { sigma[w] += sigma[v]; P[w].push(v); } } }
            while (S.length) { const w = S.pop(); for (const v of P[w]) delta[v] += (sigma[v] / sigma[w]) * (1 + delta[w]); if (w !== s) bc[w] += delta[w]; }
        }
        const scale = G.directed ? 1 : 0.5;
        const got = await G.g.betweenness({ normalized: false });
        let bad = 0;
        for (let v = 0; v < n; v++) { const want = bc[v] * scale; if (Math.abs(got[v] - want) > 1e-9 * Math.max(1, Math.abs(want))) bad++; }
        eq(bad, 0, "betweenness n=" + n + " directed=" + G.directed);
        const many = await G.g.betweenness({ normalized: false, endpoints: true });
        bad = 0;
        for (let v = 0; v < n; v++) if (!(many[v] >= got[v] - 1e-9)) bad++;
        eq(bad, 0, "betweenness endpoints >= plain n=" + n);
    }
});

__kitS.section("floydWarshall equals a JS Floyd-Warshall", () => {
    seed = 21;
    for (const G of [mk(1, true, true, 1, 1), mk(90, true, true, 3, 15), mk(150, false, true, 2, 30)]) {
        const n = G.n, d = Array.from({ length: n }, (_, i) => Array.from({ length: n }, (_, j) => (i === j ? 0 : Infinity)));
        for (const [u, v, w] of G.E) { if (w < d[u][v]) d[u][v] = w; if (!G.directed && w < d[v][u]) d[v][u] = w; }
        for (let k = 0; k < n; k++) for (let i = 0; i < n; i++) { if (d[i][k] === Infinity) continue; for (let j = 0; j < n; j++) { const nd = d[i][k] + d[k][j]; if (nd < d[i][j]) d[i][j] = nd; } }
        const got = G.g.floydWarshall();
        let bad = 0;
        for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) if (got[i][j] !== d[i][j]) bad++;
        eq(bad, 0, "floyd n=" + n + " directed=" + G.directed);
    }
});

await __kitS.section("dijkstra distances, predecessors and seeded communities match", async () => {
    seed = 31;
    const G = mk(2000, false, true, 4, 50), D = mk(1500, true, true, 3, 40);
    for (const H of [G, D]) {
        const a = adj(H, false), n = H.n, dist = new Float64Array(n).fill(Infinity), done = new Uint8Array(n);
        dist[0] = 0;
        for (;;) { let u = -1; for (let i = 0; i < n; i++) if (!done[i] && dist[i] < Infinity && (u < 0 || dist[i] < dist[u])) u = i; if (u < 0) break; done[u] = 1; for (const [x, w] of a[u]) if (dist[u] + w < dist[x]) dist[x] = dist[u] + w; }
        const got = H.g.dijkstra(0);
        let bad = 0;
        for (let i = 0; i < n; i++) if ((got[i] === undefined || got[i] === null ? Infinity : got[i]) !== dist[i] && !(dist[i] === Infinity && !(got[i] < Infinity))) bad++;
        eq(bad, 0, "dijkstra n=" + n);
        const t = H.g.shortestPathTree(0);
        bad = 0;
        for (let v = 1; v < n; v++) if (t.pred[v] >= 0 && !(a[t.pred[v]].has(v) && t.dist[t.pred[v]] + a[t.pred[v]].get(v) === t.dist[v])) bad++;
        eq(bad, 0, "spt predecessor consistency n=" + n);
    }
    const t = G.g.shortestPathTree(1);
    let ps = 0;
    for (let i = 0; i < 30; i++) ps = (ps * 31 + fp(G.g.shortestPath(i, 1999 - i).path)) | 0;
    const lv = await G.g.louvain({ seed: 3 }), lp = await G.g.labelPropagation({ seed: 9 }), lu = await small[4].g.louvain({ seed: 5 }), lpu = await small[4].g.labelPropagation({ seed: 3 });
    const got = [fp(t.pred), ps, fp(G.g.aStar(0, 1999, () => 0).path), lv.count, fp(lv.communities), fp(lp), lu.count, fp(lu.communities), fp(lpu)].join(",");
    eq(got, "1288768000,-344340288,-917320512,35,-1044070080,-1988248896,30,1320326976,-1079122048", "pre-change fingerprints (spt pred, shortestPath, aStar, louvain, labelPropagation)");
    eq(Math.abs((await G.g.modularity(lv.communities)) - lv.modularity) < 1e-9, true, "louvain modularity is the modularity of its communities");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_graph_kernels_oracle: " + checks + " checks passed");
__kitS.end();
