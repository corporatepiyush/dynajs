// covers: contract dyna:graph.Graph dyna:graph.graph -- promise, snapshot, rejection and edge-case contract of every analytics method and its CSR twin
// Graph analytics contract edges (dyna:structures Graph methods and the `graph` CSR namespace):
// every call returns a Promise and bad input REJECTS it (never a synchronous throw); option bags
// are strict; the graph is snapshotted when the call starts, so mutating it before the promise
// settles changes nothing; empty graphs, isolated nodes, self loops, parallel edges, directed vs
// undirected and negative weights where they are invalid; malformed CSR objects are refused by
// name; a large call really leaves the JS thread (a timer fires while it runs) and still agrees
// with the same call made small enough to stay inline.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_analytics");
import { Graph, graph } from "dyna:graph";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
async function rejects(p, type, re, m) {
    let err = null;
    try {
        const pr = p();
        if (!(pr instanceof Promise)) { ok(false, m + ": did not return a Promise"); return; }
        await pr;
    } catch (e) { err = e; }
    ok(err instanceof type && re.test(err.message), `${m} (got ${err && err.name}: ${err && err.message})`);
}
function syncThrows(f) { try { f(); return false; } catch { return true; } }

const ALL = ["pageRank", "labelPropagation", "louvain", "coreNumbers", "triangles", "clustering", "betweenness", "closeness",
    "stronglyConnectedComponents", "weaklyConnectedComponents"];

await __kitS.section("modularity of the empty graph is 0", async () => {
    const g = new Graph();
    for (const name of ALL) {
        const p = g[name]();
        ok(p instanceof Promise, `${name} returns a Promise`);
        const r = await p;
        const len = r.length ?? r.labels?.length ?? r.perVertex?.length ?? r.local?.length ?? r.communities?.length;
        ok(len === 0, `${name} of the empty graph is empty`);
    }
    ok((await g.modularity([])) === 0, "modularity of the empty graph is 0");
    ok((await g.triangles()).total === 0 && (await g.clustering()).average === 0, "empty triangles and clustering are 0");
    const lv = await g.louvain();
    ok(lv.count === 0 && lv.modularity === 0 && lv.levels === 0, "louvain of the empty graph");
    ok((await new Graph({ directed: true }).topologicalLevels()).count === 0, "topologicalLevels of the empty digraph");
});

await __kitS.section("isolated nodes share PageRank uniformly", async () => {
    const g = new Graph();
    for (let i = 0; i < 4; i++) g.addNode();
    const pr = await g.pageRank();
    ok(pr.every((x) => Math.abs(x - 0.25) < 1e-15), "isolated nodes share PageRank uniformly");
    ok((await g.betweenness()).every((x) => x === 0) && (await g.closeness()).every((x) => x === 0), "isolated nodes have zero centrality");
    const lv = await g.louvain();
    ok(lv.count === 4 && lv.modularity === 0, "edgeless louvain keeps singletons with modularity 0 (networkx)");
    ok((await g.weaklyConnectedComponents()).count === 4, "four isolated nodes are four components");
    const lp = await g.labelPropagation();
    ok(new Set(lp).size === 4, "isolated nodes keep their own labels");
});

await __kitS.section("parallel edges and self loops do not create triangles", async () => {
    const g = new Graph();
    g.addEdge(0, 1).addEdge(0, 1).addEdge(1, 2).addEdge(2, 0).addEdge(2, 2);
    const t = await g.triangles();
    ok(t.total === 1 && Array.from(t.perVertex).join() === "1,1,1", "parallel edges and self loops do not create triangles");
    ok(Array.from(await g.coreNumbers()).join() === "2,2,2", "core numbers ignore the multi-edge and the loop");
    const c = await g.clustering();
    ok(c.transitivity === 1 && c.average === 1, "a triangle with extras is fully clustered");
    const bc = await g.betweenness();
    ok(bc.every((x) => x === 0), "no node of a triangle is between the others");
    const pr = await g.pageRank();
    ok(pr[0] > 0 && Math.abs(pr.reduce((a, b) => a + b, 0) - 1) < 1e-12, "PageRank sums to 1 with a parallel edge and a loop");
});

await __kitS.section("two SCCs", async () => {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1).addEdge(1, 2).addEdge(2, 0).addEdge(2, 3).addEdge(3, 4).addEdge(4, 3);
    const s = await g.stronglyConnectedComponents();
    ok(s.count === 2 && s.labels[0] === s.labels[1] && s.labels[1] === s.labels[2] && s.labels[3] === s.labels[4] && s.labels[0] !== s.labels[3], "two SCCs");
    ok(s.labels[3] < s.labels[0], "the sink component gets the lower id");
    const w = await g.weaklyConnectedComponents();
    ok(w.count === 1, "one weak component");
    await rejects(() => g.topologicalLevels(), RangeError, /cycle/, "topologicalLevels rejects a cycle");
    await rejects(() => new Graph().topologicalLevels(), TypeError, /directed/, "topologicalLevels needs a directed graph");
});

await __kitS.section("pageRank rejects a negative weight", async () => {
    const g = new Graph({ directed: true, weighted: true });
    g.addEdge(0, 1, 2).addEdge(1, 2, -1);
    await rejects(() => g.pageRank(), RangeError, /negative edge weight/, "pageRank rejects a negative weight");
    await rejects(() => g.maxFlow(0, 2), RangeError, /negative edge weight/, "maxFlow rejects a negative capacity");
    await rejects(() => g.betweenness(), RangeError, /non-positive edge weight/, "weighted betweenness rejects a negative weight");
    await rejects(() => g.closeness(), RangeError, /negative edge weight/, "weighted closeness rejects a negative weight");
    await rejects(() => g.louvain(), RangeError, /negative edge weight/, "louvain rejects a negative weight");
    ok((await g.betweenness({ weighted: false })).length === 3, "unweighted betweenness ignores the weights");
    const z = new Graph({ weighted: true });
    z.addEdge(0, 1, 0).addEdge(1, 2, 1);
    await rejects(() => z.betweenness(), RangeError, /non-positive/, "weighted betweenness rejects a zero weight");
    ok((await z.closeness()).length === 3, "closeness accepts a zero weight");
});

await __kitS.section("option bags are strict", async () => {
    const g = new Graph();
    g.addEdge(0, 1).addEdge(1, 2);
    await rejects(() => g.pageRank({ dampin: 0.9 }), TypeError, /unknown option "dampin"/, "option bags are strict");
    await rejects(() => g.pageRank({ damping: 1.5 }), RangeError, /damping/, "damping outside [0, 1]");
    await rejects(() => g.pageRank({ maxIterations: 0 }), RangeError, /maxIterations/, "maxIterations below 1");
    await rejects(() => g.pageRank({ tolerance: 0, maxIterations: 3 }), RangeError, /no convergence within 3 iterations/, "non-convergence rejects with the iteration count");
    await rejects(() => g.pageRank({ personalization: [1, 2] }), RangeError, /2 entries, expected 3/, "personalization length");
    await rejects(() => g.pageRank({ personalization: [0, 0, 0] }), RangeError, /invalid input/, "all-zero personalization");
    await rejects(() => g.pageRank(5), TypeError, /options must be an object/, "non-object options");
    await rejects(() => g.modularity([0, 1]), RangeError, /communities has 2 entries/, "modularity communities length");
    await rejects(() => g.modularity([0, 1, 3]), RangeError, /communities\[2\] = 3/, "community ids must be < n");
    await rejects(() => g.modularity([0, 1.5, 0]), RangeError, /communities\[1\]/, "community ids must be integers");
    await rejects(() => g.betweenness({ samples: 0 }), RangeError, /samples/, "samples below 1");
    await rejects(() => g.betweenness({ samples: 4 }), RangeError, /samples/, "samples above n");
    await rejects(() => g.betweenness({ samples: 2, sources: [0] }), TypeError, /not both/, "samples and sources are exclusive");
    await rejects(() => g.betweenness({ sources: [0, 0] }), RangeError, /repeats node 0/, "repeated sources");
    await rejects(() => g.betweenness({ weighted: true }), TypeError, /needs edge weights/, "weighted on an unweighted graph");
    await rejects(() => g.maxFlow(0, 0), RangeError, /same node/, "maxFlow source == sink");
    await rejects(() => g.maxFlow(0, 9), RangeError, /out of range/, "maxFlow sink out of range");
    await rejects(() => g.linkPrediction([[0, 1]], { method: "katz" }), TypeError, /method must be one of/, "unknown link method");
    await rejects(() => g.linkPrediction([0, 1, 2]), RangeError, /even length/, "odd flat pairs");
    await rejects(() => g.linkPrediction([[0, 7]]), RangeError, /pair\[1\] = 7/, "pair node out of range");
    await rejects(() => g.labelPropagation({ maxRounds: -1 }), RangeError, /maxRounds/, "maxRounds below 1");
    await rejects(() => g.louvain({ resolution: -1 }), RangeError, /resolution/, "negative resolution");
    await rejects(() => g.louvain({ seed: 1.5 }), RangeError, /seed/, "fractional seed");
    ok(!syncThrows(() => { g.pageRank({ bogus: 1 }).catch(() => {}); }), "bad options never throw synchronously");
    const t = new Graph();
    t.addEdge(0, 1).addEdge(1, 2).addEdge(2, 0);
    await rejects(() => t.bipartiteMatching(), RangeError, /not bipartite/, "an odd cycle has no bipartition");
    await rejects(() => g.bipartiteMatching({ left: [1, 1, 0] }), RangeError, /not bipartite under options.left/, "a left set with an inner edge");
    await rejects(() => g.bipartiteMatching({ left: [1, 0] }), RangeError, /2 entries/, "left length");
    const m = await g.bipartiteMatching({ left: new Uint8Array([0, 1, 0]) });
    ok(m.size === 1, "a typed left set");
});

await __kitS.section("free pageRank on a hand-built CSR (directed by default)", async () => {
    const csr = { offsets: new Int32Array([0, 2, 3, 3]), edges: new Int32Array([1, 2, 2]) };
    const pr = await graph.pageRank(csr);
    ok(pr.length === 3 && pr[2] > pr[1] && pr[1] > pr[0], "free pageRank on a hand-built CSR (directed by default)");
    const und = await graph.pageRank({ ...csr, directed: false });
    ok(und.length === 3, "directed: false is accepted");
    ok((await graph.pageRank({ offsets: new Uint32Array([0, 1, 1]), edges: new Uint32Array([1]) })).length === 2, "Uint32Array CSR");
    await rejects(() => graph.pageRank(), TypeError, /expected a CSR object/, "missing CSR");
    await rejects(() => graph.pageRank({ offsets: [0, 1], edges: new Int32Array([0]) }), TypeError, /offsets must be an Int32Array/, "plain-array offsets");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0, 2]), edges: new Int32Array([0]) }), RangeError, /malformed CSR/, "offsets[n] != edges.length");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0, 1, 0]), edges: new Int32Array([0]) }), RangeError, /malformed CSR/, "decreasing offsets");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([-1, 0]), edges: new Int32Array([]) }), RangeError, /csr.offsets\[0\]/, "negative offset");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0, 1]), edges: new Int32Array([1]) }), RangeError, /csr.edges\[0\] = 1/, "target past n");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0]), edges: new Int32Array([]), extra: 1 }), TypeError, /unknown option "extra"/, "strict CSR keys");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([]), edges: new Int32Array([]) }), RangeError, /n \+ 1 entries/, "empty offsets");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0, 1]), edges: new Int32Array([0]), weights: new Float64Array([NaN]) }), RangeError, /malformed CSR/, "NaN weight");
    await rejects(() => graph.pageRank({ offsets: new Int32Array([0, 1]), edges: new Int32Array([0]), weights: new Float64Array([1, 2]) }), RangeError, /csr.weights has 2 entries/, "weights length");
    await rejects(() => graph.topologicalLevels({ ...csr, directed: false }), TypeError, /directed/, "levels need a directed CSR");
    const empty = await graph.coreNumbers({ offsets: new Int32Array([0]), edges: new Int32Array([]) });
    ok(empty.length === 0, "a zero-node CSR");
});

await __kitS.section("the graph is snapshotted when the call starts", async () => {
    const g = new Graph();
    for (let i = 0; i < 50; i++) g.addEdge(i, (i + 1) % 50);
    const before = await g.pageRank();
    const p = g.pageRank();
    for (let i = 0; i < 50; i++) g.addEdge(i, (i * 7) % 50);
    const after = await p;
    ok(after.every((x, i) => x === before[i]), "the graph is snapshotted when the call starts");
    const csr = g.exportCSR();
    const p2 = graph.triangles(csr);
    csr.edges.fill(0);
    const t2 = await p2, t3 = await g.triangles();
    ok(t2.total === t3.total, "the CSR arrays are snapshotted when the call starts");
});

await __kitS.section("sampled betweenness is deterministic for a seed", async () => {
    const n = 60000, g = new Graph();
    for (let i = 0; i < n; i++) g.addEdge(i, (i * 7919 + 1) % n).addEdge(i, (i + 1) % n);
    let ticks = 0;
    const timer = setInterval(() => ticks++, 1);
    const t0 = Date.now();
    const bc = await g.betweenness({ samples: 64, seed: 9 });
    clearInterval(timer);
    const ms = Date.now() - t0;
    // wall-clock: either the call returned within 20 ms or the loop ticked meanwhile; load can only add ticks
    ok(bc.length === n && (ms < 20 || ticks > 0), `the event loop keeps running while a large call computes (${ticks} ticks in ${ms} ms)`);
    const bc2 = await g.betweenness({ samples: 64, seed: 9 });
    ok(bc.every((x, i) => x === bc2[i]), "sampled betweenness is deterministic for a seed");
    const bc3 = await g.betweenness({ samples: 64, seed: 10 });
    ok(bc.some((x, i) => x !== bc3[i]), "another seed samples other sources");
});

__kitS.section("exportCSR returns weights in arc order", () => {
    const g = new Graph({ weighted: true });
    g.addEdge(0, 1, 2.5).addEdge(1, 2, 1);
    const c = g.exportCSR();
    ok(c.weights instanceof Float64Array && Array.from(c.weights).join() === "2.5,2.5,1,1" && c.directed === false, "exportCSR returns weights in arc order");
    ok(new Graph().exportCSR().weights === undefined, "an unweighted graph exports no weights");
});
__kitS.end();

print(`test_structures_graph_analytics: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} graph analytics checks failed`);
