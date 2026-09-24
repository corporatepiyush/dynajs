// flags: --std
/* test_structures_graph_into.js — (exportCSR / neighborsInto) and
 * (visitor traversals bfs/dfs/topologicalSort) for dyna:structures Graph.
 *
 * Oracles are networkx-free graph invariants computed in plain JS:
 *   - exportCSR vs neighbors() per node (offsets prefix sums, edge multisets)
 *   - visitor traversal vs the array forms (same order, early exit = prefix)
 *   - allocation contracts exercised at 1M edges (no per-hop JS garbage)
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_structures_graph_into.js */

import { Graph } from "dyna:structures";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
function throws(fn, m) { let t = false; try { fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

/* ----------------: exportCSR ---------------- */
{
    const g = new Graph({ directed: true });
    for (let i = 0; i < 6; i++) g.addNode();
    g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3); g.addEdge(3, 4); g.addEdge(2, 4);
    const csr = g.exportCSR();
    assert(csr.offsets instanceof Int32Array && csr.edges instanceof Int32Array,
        "exportCSR returns Int32Arrays");
    assert(csr.offsets.length === g.nodeCount + 1, "offsets is n+1");
    assert(eqArr([...csr.offsets], [0, 2, 3, 4, 5, 5, 5]), "offsets prefix sums");
    assert(eqArr([...csr.edges], [1, 2, 3, 4, 4]), "edges node-ordered");
    /* edges[offsets[u]..offsets[u+1]) === neighbors(u), for every node */
    for (let u = 0; u < g.nodeCount; u++)
        assert(eqArr([...csr.edges.slice(csr.offsets[u], csr.offsets[u + 1])],
                     g.neighbors(u)), `csr slice == neighbors(${u})`);
    /* self-loops appear once, as in neighbors() */
    g.addEdge(4, 4);
    const c2 = g.exportCSR();
    assert(eqArr([...c2.edges.slice(c2.offsets[4], c2.offsets[5])], g.neighbors(4)),
        "self-loop appears once in csr");
    /* fresh arrays each call: mutating a result cannot corrupt the graph */
    const a1 = g.exportCSR(), a2 = g.exportCSR();
    a1.edges[0] = 999;
    assert(a2.edges[0] === 1, "exportCSR copies into fresh arrays");
    assert(g.neighbors(0)[0] === 1, "graph untouched by result mutation");
}

/* exportCSR on degenerate shapes */
{
    const empty = new Graph();
    const ce = empty.exportCSR();
    assert(ce.offsets.length === 1 && ce.offsets[0] === 0 && ce.edges.length === 0,
        "empty graph: offsets=[0], edges=[]");
    const one = new Graph();
    one.addNode();
    const c1 = one.exportCSR();
    assert(eqArr([...c1.offsets], [0, 0]) && c1.edges.length === 0, "one isolated node");
    const iso = new Graph({ directed: true });
    iso.addEdge(0, 5);          /* gaps become isolated nodes 1..4 */
    const ci = iso.exportCSR();
    assert(ci.offsets.length === 7 && eqArr([...ci.edges], [5]), "gaps are isolated rows");
}

/* randomized differential: CSR vs the adjacency it came from */
{
    const rnd = mulberry32(0xC5F00D);
    const g = new Graph({ directed: true });
    const N = 200;
    for (let i = 0; i < N; i++) g.addNode();
    for (let e = 0; e < 1500; e++)
        g.addEdge((rnd() * N) | 0, (rnd() * N) | 0);
    const csr = g.exportCSR();
    let tot = 0;
    for (let u = 0; u < N; u++) {
        assert(csr.offsets[u] === tot, `offset monotone at ${u}`);
        tot += g.neighbors(u).length;
        assert(csr.offsets[u + 1] === tot, `offset end at ${u}`);
    }
    assert(csr.edges.length === tot && tot === 1500, "total degree == edgeCount");
    for (let u = 0; u < N; u++)
        assert(eqArr([...csr.edges.slice(csr.offsets[u], csr.offsets[u + 1])],
                     g.neighbors(u)), `random csr row ${u}`);
}

/* ----------------: neighborsInto ---------------- */
{
    const g = new Graph({ directed: true });
    g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(0, 3);
    const out = new Int32Array(8);
    assert(g.neighborsInto(0, out) === 3, "returns the degree");
    assert(eqArr([...out.slice(0, 3)], [1, 2, 3]), "wrote the neighbors");
    assert(out[3] === 0, "extra capacity untouched");
    assert(eqArr((() => { const o = new Int32Array(0);
        return [g.neighborsInto(1, o), o.length]; })(), [0, 0]),
        "sink node: zero neighbors, zero writes");
    /* shorter than the degree: RangeError BEFORE any write */
    const small = new Int32Array(2);
    throws(() => g.neighborsInto(0, small), "short out throws RangeError");
    assert(eqArr([...small], [0, 0]), "nothing written on the short-out path");
    throws(() => g.neighborsInto(99, new Int32Array(4)), "out-of-range node throws");
    throws(() => g.neighborsInto(0, new Float64Array(4)), "non-Int32Array throws");
    /* node ids land in the caller's buffer (no copy of the array itself) */
    const buf = new Int32Array(4);
    g.neighborsInto(0, buf);
    buf[0] = 42;
    assert(g.neighbors(0)[0] === 1, "graph state is independent of out buffer");
}

/* ----------------: visitor traversals ---------------- */
{
    const g = new Graph({ directed: true });
    g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3); g.addEdge(3, 4); g.addEdge(2, 4);
    /* full walk (never stops) visits exactly the array form's order */
    for (const [kind, arr] of [["bfs", g.bfs(0)], ["dfs", g.dfs(0)]]) {
        const seen = [];
        const rc = g[kind](0, { visit(u) { seen.push(u); return false; } });
        assert(rc === arr.length, `${kind} visitor returns the visited count`);
        assert(eqArr(seen, [...arr]), `${kind} visitor order == array order`);
    }
    /* early exit: the visited set is a prefix of the full order, and the count
     * matches (the node whose visit returned true IS counted) */
    {
        const full = g.bfs(0);
        const seen = [];
        const rc = g.bfs(0, { visit(u) { seen.push(u); return u === full[2]; } });
        assert(rc === 3 && eqArr(seen, full.slice(0, 3)), "bfs early exit is a prefix");
    }
    {
        const full = g.dfs(0);
        const seen = [];
        const rc = g.dfs(0, { visit(u) { seen.push(u); return u === full[1]; } });
        assert(rc === 2 && eqArr(seen, full.slice(0, 2)), "dfs early exit is a prefix");
    }
    /* start node early-exit: visit(src) -> true visits exactly one node */
    assert(g.bfs(0, { visit() { return true; } }) === 1, "bfs immediate stop");
    assert(g.dfs(0, { visit() { return true; } }) === 1, "dfs immediate stop");
    /* undefined return keeps going (boolean|void contract) */
    {
        let steps = 0;
        const rc = g.bfs(0, { visit() { steps++; if (steps === 3) return true; } });
        assert(rc === 3 && steps === 3, "undefined treated as continue");
    }
    /* source out of range still throws before any visit */
    throws(() => g.bfs(99, { visit() { return false; } }), "bfs visitor range check");
    throws(() => g.dfs(99, { visit() { return false; } }), "dfs visitor range check");
    /* reachable-only: isolated nodes are never visited */
    {
        let visits = 0;
        g.bfs(0, { visit() { visits++; return false; } });
        assert(visits === 5, "unreachable nodes not visited");
    }
}

/* visitor throw propagates and leaves the graph usable */
{
    const g = new Graph({ directed: true });
    for (let i = 0; i < 50; i++) g.addNode();
    for (let u = 0; u < 49; u++) g.addEdge(u, u + 1);
    const boom = new Error("mid-traversal");
    for (const kind of ["bfs", "dfs"]) {
        let threw = null, after = 0;
        try { g[kind](0, { visit(u) { if (u === 25) throw boom; return false; } }); }
        catch (e) { threw = e; }
        assert(threw === boom, `${kind} visitor throw propagates the same object`);
        after = g[kind](0).length;
        assert(after === 50, `${kind} traversal state cleaned up after throw`);
    }
    /* throwing topologicalSort visitor */
    {
        const dag = new Graph({ directed: true });
        dag.addEdge(0, 1); dag.addEdge(1, 2);
        let threw = false;
        try { dag.topologicalSort({ visit(u) { if (u === 1) throw boom; return false; } }); }
        catch (e) { threw = e === boom; }
        assert(threw, "topologicalSort visitor throw propagates");
        assert(eqArr(dag.topologicalSort(), [0, 1, 2]), "topo state intact after throw");
    }
}

/* a visitor that MUTATES the graph is refused (the working set is sized once,
 * the same discipline aStar applies to a mutating heuristic); the graph stays
 * usable and consistent afterwards */
{
    const g = new Graph({ directed: true });
    for (let i = 0; i < 30; i++) g.addNode();
    for (let u = 0; u < 29; u++) g.addEdge(u, u + 1);
    for (const kind of ["bfs", "dfs"]) {
        let msg = "";
        try { g[kind](0, { visit() { g.addNode(); return false; } }); }
        catch (e) { msg = e.message; }
        assert(msg.includes("mutated the graph"), `${kind} refuses a mutating visitor`);
    }
    {
        let msg = "";
        try { g.topologicalSort({ visit() { g.addEdge(0, 29); return false; } }); }
        catch (e) { msg = e.message; }
        assert(msg.includes("mutated the graph"), "topo refuses a mutating visitor");
    }
    assert(g.nodeCount === 32 && g.edgeCount === 30,
        "post-refusal graph is consistent (mutations already applied)");
    assert(g.bfs(0).length === 30, "traversal still works after refusals");
}

/* topologicalSort visitor */
{
    const dag = new Graph({ directed: true });
    dag.addEdge(0, 1); dag.addEdge(1, 2); dag.addEdge(0, 2);
    const order = dag.topologicalSort();
    const seen = [];
    const rc = dag.topologicalSort({ visit(u) { seen.push(u); return false; } });
    assert(rc === order.length && eqArr(seen, order), "topo visitor == array order");
    assert(dag.topologicalSort({ visit() { return true; } }) === 1, "topo early stop");
    /* a cycle reached BEFORE the early exit still throws */
    const cyc = new Graph({ directed: true });
    cyc.addEdge(0, 1); cyc.addEdge(1, 0);
    throws(() => cyc.topologicalSort({ visit() { return false; } }), "cycle still throws");
    /* early exit before the cycle is reached: caller's choice, not an error */
    {
        const late = new Graph({ directed: true });
        late.addEdge(0, 1); late.addEdge(1, 2); late.addEdge(2, 1);  /* 1<->2 cycle */
        assert(late.topologicalSort({ visit(u) { return u === 0; } }) === 1,
            "early exit skips the cycle check");
        throws(() => late.topologicalSort({ visit() { return false; } }),
            "same graph throws when the walk runs past the cycle");
    }
    throws(() => new Graph().topologicalSort({ visit() { return false; } }),
        "topo visitor still requires directed");
    /* strict bag */
    throws(() => dag.bfs(0, { visit() {}, extra: 1 }), "unknown visitor option throws");
    throws(() => dag.bfs(0, {}), "missing visit throws");
    throws(() => dag.bfs(0, { visit: 42 }), "non-function visit throws");
    throws(() => dag.topologicalSort({ visi() {} }), "typo'd visit throws");
    /* bfs(src, undefined/null) keeps the array form */
    assert(eqArr(dag.bfs(0, undefined), [0, 1, 2]), "bfs(src, undefined) is the array form");
    assert(eqArr(dag.topologicalSort(undefined), [0, 1, 2]), "topo(undefined) array form");
}

/* ---------------- scale: 1M edges, flat per-hop cost ---------------- */
{
    const N = 100000, EDGE_MUL = 10;
    const g = new Graph({ directed: true });
    for (let i = 0; i < N; i++) g.addNode();
    for (let k = 0; k < EDGE_MUL; k++)
        for (let u = 0; u < N; u++)
            g.addEdge(u, (u * 7 + k * 13 + 1) % N);
    assert(g.edgeCount === N * EDGE_MUL, "1M logical edges");
    const t0 = Date.now();
    const csr = g.exportCSR();
    const t1 = Date.now();
    assert(csr.offsets.length === N + 1 && csr.edges.length === N * EDGE_MUL,
        "csr sized at scale");
    /* CSR walk by hand: every node reachable in this construction */
    const seen = new Uint8Array(N);
    let count = 0;
    const stack = [0]; seen[0] = 1;
    while (stack.length) {
        const u = stack.pop();
        count++;
        for (let p = csr.offsets[u]; p < csr.offsets[u + 1]; p++) {
            const v = csr.edges[p];
            if (!seen[v]) { seen[v] = 1; stack.push(v); }
        }
    }
    assert(count === N, "csr-fed DFS reaches every node");
    /* visitor traversal at scale, and neighborsInto without allocation */
    let m = 0;
    const rc = g.bfs(0, { visit() { m++; return false; } });
    assert(rc === m, "visitor count consistent at scale");
    const buf = new Int32Array(EDGE_MUL);
    assert(g.neighborsInto(5, buf) === EDGE_MUL, "neighborsInto at scale");
    const t2 = Date.now();
    assert(t2 - t0 < 10000, `scale probe fast enough (csr ${t1 - t0}ms, walk ${t2 - t1}ms)`);
}

/* serialization round-trip keeps the new methods working on a restored graph */
{
    const g = new Graph({ directed: true });
    g.addEdge(0, 1); g.addEdge(1, 2);
    const r = Graph.deserialize(g.serialize());
    assert(eqArr([...r.exportCSR().edges], [1, 2]), "exportCSR after deserialize");
    assert(r.bfs(0, { visit() { return true; } }) === 1, "visitor after deserialize");
}

console.log(`test_structures_graph_into: ${n} assertions ok`);
