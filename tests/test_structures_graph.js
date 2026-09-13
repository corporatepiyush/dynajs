/* test_structures_graph.js — dyna:structures Graph + algorithms as methods.
 * Value matrices, randomized differential oracles (vs pure-JS BFS/Dijkstra/
 * Kruskal/union-find), error paths, and GC lifecycle (plain object, no close).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_structures_graph.js */

import { Graph } from "dyna:structures";
import * as std from "std";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
function throws(fn, m) { let t = false; try { fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
const INF = Infinity;

/* ---------------- construction / addNode / addEdge / neighbors ---------------- */
{
    const g = new Graph({ directed: true, weighted: true });
    const a = g.addNode(), b = g.addNode(), c = g.addNode();
    assert(a === 0 && b === 1 && c === 2, "addNode returns sequential ids");
    assert(g.nodeCount === 3 && g.edgeCount === 0, "counts");
    assert(g.addEdge(a, b, 5) === g, "addEdge chainable");
    g.addEdge(b, c, 2);
    assert(g.edgeCount === 2, "edge count");
    assert(eqArr(g.neighbors(a), [b]), "neighbors directed");
    assert(eqArr(g.neighbors(c), []), "sink has no out-neighbors");
    assert(g.hasEdge(a, b) && !g.hasEdge(b, a), "directed hasEdge");
    /* addEdge auto-grows nodes */
    g.addEdge(5, 6, 1);
    assert(g.nodeCount === 7, "addEdge auto-creates endpoints");
    throws(() => g.addEdge(-1, 0), "negative node id throws");
    throws(() => g.neighbors(99), "neighbors out of range throws");
}

/* undirected adds both directions; unweighted forces weight 1 */
{
    const g = new Graph();  /* undirected, unweighted */
    g.addEdge(0, 1); g.addEdge(1, 2);
    assert(g.hasEdge(0, 1) && g.hasEdge(1, 0), "undirected both directions");
    assert(g.edgeCount === 2, "logical edge count");
    assert(eqArr(g.dijkstra(0), [0, 1, 2]), "unweighted distances are hop counts");
}

/* ---------------- BFS / DFS reachability ---------------- */
{
    const g = new Graph({ directed: true });
    g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3); g.addEdge(4, 5);
    const bfs = g.bfs(0), dfs = g.dfs(0);
    assert(eqArr([...bfs].sort((a, b) => a - b), [0, 1, 2, 3]), "bfs reaches 0..3");
    assert(eqArr([...dfs].sort((a, b) => a - b), [0, 1, 2, 3]), "dfs reaches 0..3");
    assert(bfs[0] === 0 && dfs[0] === 0, "traversal starts at src");
    assert(!bfs.includes(4) && !bfs.includes(5), "unreachable nodes absent");
    throws(() => g.bfs(99), "bfs source out of range throws");
}

/* ---------------- Dijkstra + differential vs a JS oracle ---------------- */
function jsDijkstra(nAdj, src) {   /* nAdj: array of [ [to,w], ... ] per node */
    const N = nAdj.length, dist = new Array(N).fill(INF); dist[src] = 0;
    const done = new Array(N).fill(false);
    for (let it = 0; it < N; it++) {
        let u = -1, best = INF;
        for (let i = 0; i < N; i++) if (!done[i] && dist[i] < best) { best = dist[i]; u = i; }
        if (u === -1) break;
        done[u] = true;
        for (const [v, w] of nAdj[u]) if (dist[u] + w < dist[v]) dist[v] = dist[u] + w;
    }
    return dist;
}
{
    const g = new Graph({ directed: true, weighted: true });
    g.addEdge(0, 1, 7).addEdge(0, 2, 9).addEdge(0, 5, 14).addEdge(1, 2, 10)
     .addEdge(1, 3, 15).addEdge(2, 3, 11).addEdge(2, 5, 2).addEdge(3, 4, 6)
     .addEdge(5, 4, 9);
    const d = g.dijkstra(0);
    assert(d[0] === 0 && d[1] === 7 && d[2] === 9 && d[3] === 20, "dijkstra known");
    assert(d[4] === 20 && d[5] === 11, "dijkstra known (via 2)");
    assert(g.dijkstra(0, 4) === 20, "dijkstra with dst");
    /* unreachable */
    const g2 = new Graph({ directed: true, weighted: true });
    g2.addEdge(0, 1, 1); g2.addNode(); g2.addNode(); /* nodes 2,3 unreachable */
    assert(g2.dijkstra(0)[3] === INF, "unreachable => Infinity");
    /* negative edge rejected */
    const g3 = new Graph({ directed: true, weighted: true });
    g3.addEdge(0, 1, -1);
    throws(() => g3.dijkstra(0), "dijkstra rejects negative weight");
}
{
    /* differential vs JS Dijkstra over random weighted digraphs */
    const rng = mulberry32(5);
    for (let seed = 0; seed < 30; seed++) {
        const N = 3 + ((rng() * 20) | 0);
        const g = new Graph({ directed: true, weighted: true });
        for (let i = 0; i < N; i++) g.addNode();
        const adj = Array.from({ length: N }, () => []);
        const E = (rng() * N * 2) | 0;
        for (let e = 0; e < E; e++) {
            const u = (rng() * N) | 0, v = (rng() * N) | 0, w = 1 + ((rng() * 20) | 0);
            g.addEdge(u, v, w); adj[u].push([v, w]);
        }
        const src = (rng() * N) | 0;
        const got = g.dijkstra(src), want = jsDijkstra(adj, src);
        for (let i = 0; i < N; i++)
            assert(got[i] === want[i], "dijkstra agrees @seed " + seed + " node " + i);
        /* bellmanFord must match dijkstra on non-negative graphs */
        const bf = g.bellmanFord(src);
        for (let i = 0; i < N; i++)
            assert(bf[i] === want[i], "bellmanFord agrees @seed " + seed + " node " + i);
    }
}

/* ---------------- Bellman-Ford: negative edges ok, negative cycle throws ---- */
{
    const g = new Graph({ directed: true, weighted: true });
    g.addEdge(0, 1, 4).addEdge(0, 2, 5).addEdge(1, 2, -3).addEdge(2, 3, 2);
    const d = g.bellmanFord(0);
    assert(d[2] === 1 && d[3] === 3, "bellmanFord with a negative edge");
    const cyc = new Graph({ directed: true, weighted: true });
    cyc.addEdge(0, 1, 1).addEdge(1, 2, -1).addEdge(2, 0, -1);   /* net -1 cycle */
    throws(() => cyc.bellmanFord(0), "negative cycle throws");
}

/* ---------------- topologicalSort ---------------- */
{
    const g = new Graph({ directed: true });
    g.addEdge(5, 2).addEdge(5, 0).addEdge(4, 0).addEdge(4, 1).addEdge(2, 3).addEdge(3, 1);
    const order = g.topologicalSort();
    assert(order.length === 6, "topo covers all nodes");
    const pos = {}; order.forEach((x, i) => pos[x] = i);
    for (const [u, v] of [[5, 2], [5, 0], [4, 0], [4, 1], [2, 3], [3, 1]])
        assert(pos[u] < pos[v], "topo respects edge " + u + "->" + v);
    const cyc = new Graph({ directed: true });
    cyc.addEdge(0, 1).addEdge(1, 2).addEdge(2, 0);
    throws(() => cyc.topologicalSort(), "cyclic topo throws");
    throws(() => new Graph().topologicalSort(), "undirected topo throws");
}

/* ---------------- connectedComponents (differential vs union-find) ---------- */
{
    const rng = mulberry32(13);
    for (let seed = 0; seed < 20; seed++) {
        const N = 2 + ((rng() * 25) | 0);
        const g = new Graph();  /* undirected */
        for (let i = 0; i < N; i++) g.addNode();
        const parent = Array.from({ length: N }, (_, i) => i);
        const find = x => { while (parent[x] !== x) x = parent[x] = parent[parent[x]]; return x; };
        const E = (rng() * N) | 0;
        for (let e = 0; e < E; e++) {
            const u = (rng() * N) | 0, v = (rng() * N) | 0;
            g.addEdge(u, v); parent[find(u)] = find(v);
        }
        const comp = g.connectedComponents();
        /* two nodes share a component iff they share a union-find root */
        for (let i = 0; i < N; i++)
            for (let j = i + 1; j < N; j++)
                assert((comp[i] === comp[j]) === (find(i) === find(j)),
                       "component agreement @seed " + seed);
    }
}

/* ---------------- floydWarshall vs per-source Dijkstra ---------------- */
{
    const rng = mulberry32(23);
    for (let seed = 0; seed < 12; seed++) {
        const N = 2 + ((rng() * 12) | 0);
        const g = new Graph({ directed: true, weighted: true });
        for (let i = 0; i < N; i++) g.addNode();
        const E = (rng() * N * 2) | 0;
        for (let e = 0; e < E; e++)
            g.addEdge((rng() * N) | 0, (rng() * N) | 0, 1 + ((rng() * 10) | 0));
        const fw = g.floydWarshall();
        for (let s = 0; s < N; s++) {
            const dk = g.dijkstra(s);
            for (let t = 0; t < N; t++)
                assert(fw[s][t] === dk[t], "floyd==dijkstra @seed " + seed + " " + s + "->" + t);
        }
    }
}

/* ---------------- mst (differential vs a JS Kruskal) ---------------- */
{
    const rng = mulberry32(29);
    for (let seed = 0; seed < 20; seed++) {
        const N = 2 + ((rng() * 15) | 0);
        const g = new Graph({ weighted: true });  /* undirected */
        for (let i = 0; i < N; i++) g.addNode();
        const edges = [];
        const E = (rng() * N * 2) | 0;
        for (let e = 0; e < E; e++) {
            let u = (rng() * N) | 0, v = (rng() * N) | 0;
            if (u === v) continue;
            const w = 1 + ((rng() * 30) | 0);
            g.addEdge(u, v, w); edges.push([u, v, w]);
        }
        /* JS Kruskal oracle */
        edges.sort((a, b) => a[2] - b[2]);
        const par = Array.from({ length: N }, (_, i) => i);
        const find = x => { while (par[x] !== x) x = par[x] = par[par[x]]; return x; };
        let wantW = 0;
        for (const [u, v, w] of edges) { const a = find(u), b = find(v);
            if (a !== b) { par[a] = b; wantW += w; } }
        assert(g.mst().weight === wantW, "mst weight agrees @seed " + seed);
    }
    throws(() => new Graph({ directed: true }).mst(), "mst on directed throws");
}

/* ---------------- aStar ---------------- */
{
    const g = new Graph({ weighted: true });
    g.addEdge(0, 1, 1).addEdge(1, 2, 1).addEdge(0, 2, 5).addEdge(2, 3, 1);
    /* admissible heuristic => optimal cost == dijkstra */
    for (const h of [() => 0, (x) => (3 - x) * 0.5]) {
        const r = g.aStar(0, 3, h);
        assert(r.dist === g.dijkstra(0, 3), "aStar optimal == dijkstra");
        assert(eqArr(r.path, [0, 1, 2, 3]), "aStar path");
    }
    /* unreachable */
    const g2 = new Graph({ directed: true, weighted: true });
    g2.addEdge(0, 1, 1); g2.addNode(); g2.addNode();  /* node 3 */
    const r2 = g2.aStar(0, 3, () => 0);
    assert(r2.dist === INF && r2.path.length === 0, "aStar unreachable");
    /* a heuristic that mutates the graph is rejected */
    const g3 = new Graph({ weighted: true });
    g3.addEdge(0, 1, 1).addEdge(1, 2, 1);
    throws(() => g3.aStar(0, 2, (x) => { g3.addNode(); return 0; }),
           "aStar rejects a graph-mutating heuristic");
    throws(() => g.aStar(0, 2, 42), "aStar non-function heuristic throws");
    throws(() => g.aStar(0, 99, () => 0), "aStar node out of range throws");
}

/* =====================================================================
 * GC lifecycle — plain object, no close surface
 * ===================================================================== */
{
    const g = new Graph();
    assert(g.close === undefined && g.closed === undefined &&
           g[Symbol.dispose] === undefined, "Graph has no resource surface");
    assert(typeof g === "object" && g instanceof Graph, "plain object");
}
/* finalizer reclamation: build many graphs, keep none, gc (ASan-validated). */
{
    for (let i = 0; i < 3000; i++) {
        const g = new Graph({ directed: (i & 1) === 0, weighted: true });
        for (let k = 0; k < 8; k++) g.addEdge(k, (k + 1 + i) % 8, (i + k) & 15);
        g.bfs(0); g.dijkstra(0);
    }
    std.gc();
}

print("test_structures_graph: all tests passed (" + n + " assertions)");
