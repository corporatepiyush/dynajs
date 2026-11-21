// timeout: 300
// covers: dyna:graph.Graph
// Graph typed edges (type ids 0..31 per edge) and type-mask traversal: bfs/dfs (array and visitor),
// shortestPath, neighborhood (k-hop), pathEndpoints and pathsInto (per-hop masks), exportCSR types,
// Graph.fromEdges, and the typed record. Differential against a JS reference walk and against the
// same query on a graph rebuilt from the admitted edges only, over seeded random multigraphs with
// self loops and parallel edges of different types. Records captured from the build before typed
// edges existed are embedded and must load and re-serialize byte-identically.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_typed");
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const J = (x) => JSON.stringify(x, (k, v) => ArrayBuffer.isView(v) ? Array.from(v) : (v === Infinity ? "Inf" : v));
const eqJ = (got, want, w) => { const a = J(got), b = J(want); ok(a === b, w, a === b ? "" : "got " + a.slice(0, 300) + " want " + b.slice(0, 300)); };
const throws = (fn, Ctor, re, w) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
};
const hex = (u) => Array.from(u, b => b.toString(16).padStart(2, "0")).join("");
const unhex = (s) => Uint8Array.from(s.match(/../g), h => parseInt(h, 16));

function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function randomGraph(seed) {
    const r = rng(seed);
    const n = 1 + Math.floor(r() * 24);
    const directed = r() < 0.5, weighted = r() < 0.5;
    const palette = r() < 0.5 ? [0, 1, 2, 31] : null;
    const m = Math.floor(r() * 70);
    const edges = [];
    for (let i = 0; i < m; i++) {
        const u = Math.floor(r() * n), v = r() < 0.1 ? u : Math.floor(r() * n);
        const t = palette ? palette[Math.floor(r() * palette.length)] : Math.floor(r() * 32);
        const w = weighted ? Math.floor(r() * 9) + 1 : 1;
        edges.push([u, v, w, t]);
        if (r() < 0.15) edges.push([u, v, w + 1, (t + 1 + Math.floor(r() * 30)) % 32]);
    }
    return { n, directed, weighted, edges, r };
}
function build(spec, keep) {
    const g = new Graph({ directed: spec.directed, weighted: spec.weighted });
    for (let i = 0; i < spec.n; i++) g.addNode();
    for (const [u, v, w, t] of spec.edges) if (!keep || keep(t)) g.addEdge(u, v, spec.weighted ? w : undefined, t);
    return g;
}
function refAdj(spec) {
    const adj = Array.from({ length: spec.n }, () => []);
    for (const [u, v, w, t] of spec.edges) {
        adj[u].push({ to: v, t, w });
        if (!spec.directed && u !== v) adj[v].push({ to: u, t, w });
    }
    return adj;
}
const admits = (mask, t) => ((mask >>> 0) >>> t & 1) === 1;
function refBfs(adj, s, mask) {
    const seen = new Set([s]), q = [s];
    for (let h = 0; h < q.length; h++) for (const e of adj[q[h]]) if (!seen.has(e.to) && admits(mask, e.t)) { seen.add(e.to); q.push(e.to); }
    return q;
}
function refDfs(adj, s, mask) {
    const seen = new Set(), st = [s], out = [];
    while (st.length) {
        const x = st.pop();
        if (seen.has(x)) continue;
        seen.add(x); out.push(x);
        for (let i = adj[x].length - 1; i >= 0; i--) if (!seen.has(adj[x][i].to) && admits(mask, adj[x][i].t)) st.push(adj[x][i].to);
    }
    return out;
}
function refHop(adj, s, k, mask) {
    const d = new Map([[s, 0]]), q = [s];
    for (let h = 0; h < q.length; h++) {
        const u = q[h];
        if (d.get(u) >= k) continue;
        for (const e of adj[u]) if (!d.has(e.to) && admits(mask, e.t)) { d.set(e.to, d.get(u) + 1); q.push(e.to); }
    }
    return { nodes: q, dist: q.map(x => d.get(x)) };
}
function refEnds(adj, srcs, hops) {
    let cur = new Set(srcs);
    for (const m of hops) {
        const nx = new Set();
        for (const u of cur) for (const e of adj[u]) if (admits(m, e.t)) nx.add(e.to);
        cur = nx;
    }
    return [...cur].sort((a, b) => a - b);
}
function refPaths(adj, srcs, hops) {
    const rows = [];
    const rec = (path, types) => {
        const d = types.length;
        if (d === hops.length) { rows.push({ nodes: path.slice(), types: types.slice() }); return; }
        for (const e of adj[path[d]]) if (admits(hops[d], e.t)) { path.push(e.to); types.push(e.t); rec(path, types); path.pop(); types.pop(); }
    };
    for (const s of srcs) rec([s], []);
    return rows;
}
function pickMask(r) {
    const k = r();
    if (k < 0.1) return 0;
    if (k < 0.2) return -1;
    if (k < 0.25) return 1 << 31;
    if (k < 0.45) return 1 << Math.floor(r() * 32);
    if (k < 0.65) return (1 << Math.floor(r() * 32)) | (1 << Math.floor(r() * 4));
    return Math.floor(r() * 4294967296);
}

print("== Graph typed edges ==");

__kitS.section("worked example: typed knowledge graph", () => {
    const CITES = 0, AUTHORED = 1, FUNDED = 2;
    const g = new Graph({ directed: true });
    g.addEdge(0, 1, undefined, CITES).addEdge(0, 2, undefined, CITES).addEdge(1, 3, undefined, AUTHORED)
        .addEdge(2, 4, undefined, AUTHORED).addEdge(2, 3, undefined, FUNDED).addEdge(1, 3, undefined, CITES);
    eqJ(g.pathEndpoints(0, [1 << CITES, 1 << AUTHORED]), [3, 4], "paper-[cites]->paper-[authoredBy]->author");
    eqJ(g.pathEndpoints(0, [1 << CITES, 1 << FUNDED]), [3], "cites then fundedBy");
    eqJ(g.pathEndpoints(0, [1 << AUTHORED]), [], "no authoredBy arc leaves 0");
    eqJ(g.pathEndpoints([0, 0, 1], []), [0, 1], "no hops: the distinct sources, ascending");
    const nodes = new Int32Array(30), types = new Uint8Array(20);
    const total = g.pathsInto(0, [1 << CITES, (1 << AUTHORED) | (1 << CITES)], nodes, types);
    eqJ(total, 3, "three matching walks");
    eqJ(Array.from(nodes.slice(0, 9)), [0, 1, 3, 0, 1, 3, 0, 2, 4], "walk rows, parallel 1->3 arcs give two rows");
    eqJ(Array.from(types.slice(0, 6)), [0, 1, 0, 0, 0, 1], "types tell the parallel arcs apart");
    eqJ(g.bfs(0, { types: 1 << CITES }), [0, 1, 2, 3], "bfs over cites");
    eqJ(g.bfs(0, { types: 0 }), [0], "empty mask visits only the source");
    eqJ(g.bfs(0, { types: -1 }), g.bfs(0), "all-types mask equals the unmasked walk");
    eqJ(g.shortestPath(0, 4, { types: 1 << CITES }), { dist: "Inf", path: [] }, "no cites-only path to 4");
    eqJ(g.shortestPath(0, 4, { types: 3 }), { dist: 2, path: [0, 2, 4] }, "cites|authored path");
    eqJ(g.shortestPath(0, 0, { types: 0 }), { dist: 0, path: [0] }, "src === dst under the empty mask");
    eqJ(g.neighborhood(0, 1), { nodes: [0, 1, 2], dist: [0, 1, 1] }, "1-hop");
    eqJ(g.neighborhood(0, 2, { types: 1 << CITES }), { nodes: [0, 1, 2, 3], dist: [0, 1, 1, 2] }, "2-hop over cites");
    eqJ(g.neighborhood(0, 0), { nodes: [0], dist: [0] }, "0-hop is the source");
    const csr = g.exportCSR();
    eqJ(csr.types, [0, 0, 1, 0, 1, 2], "exportCSR types aligned with edges");
    eqJ(Object.keys(csr), ["offsets", "edges", "types", "directed"], "typed exportCSR keys");
});

__kitS.section("untyped graphs: type 0 everywhere, no types key, same bytes", () => {
    const g = new Graph();
    g.addEdge(0, 1).addEdge(1, 2).addEdge(2, 0, undefined, 0);
    eqJ(Object.keys(g.exportCSR()), ["offsets", "edges", "directed"], "all-zero types export as untyped");
    eqJ(g.bfs(0, { types: 1 }), [0, 1, 2], "untyped edges are type 0");
    eqJ(g.bfs(0, { types: 2 }), [0], "a mask without bit 0 admits no untyped edge");
    eqJ(g.bfs(0, { types: 1 << 31 }), [0], "bit 31 only");
    eqJ(g.bfs(0, { types: 4294967295 }), [0, 1, 2], "0xFFFFFFFF admits all");
    eqJ(g.bfs(0, { types: -2147483647 }), [0, 1, 2], "negative int32 mask taken mod 2^32 (bit 0 set)");
    const h = new Graph();
    h.addEdge(0, 1).addEdge(1, 2).addEdge(2, 0);
    eqJ(hex(g.serialize()), hex(h.serialize()), "explicit type 0 serializes as untyped");
    const w = new Graph({ weighted: true });
    w.addEdge(0, 1, 2.5, 7).addEdge(1, 1, 1, 3);
    eqJ(w.exportCSR().types, [7, 7, 3], "undirected edge carries its type on both arcs; self loop once");
});

__kitS.section("differential: seeded random multigraphs vs JS reference and filtered rebuild", () => {
    let bad = 0, checks = 0;
    const note = (c, what, seed, d) => { checks++; if (!c && bad++ < 6) print("  seed " + seed + " " + what + (d ? " " + d : "")); };
    for (let seed = 1; seed <= 300; seed++) {
        const spec = randomGraph(seed), r = spec.r, adj = refAdj(spec);
        const g = build(spec);
        const f = Graph.fromEdges(Int32Array.from(spec.edges, e => e[0]), Int32Array.from(spec.edges, e => e[1]),
            { directed: spec.directed, weighted: spec.weighted, nodes: spec.n, types: Uint8Array.from(spec.edges, e => e[3]),
              ...(spec.weighted ? { weights: Float64Array.from(spec.edges, e => e[2]) } : {}) });
        note(J(f.exportCSR()) === J(g.exportCSR()) && f.edgeCount === g.edgeCount && f.nodeCount === g.nodeCount, "fromEdges == addEdge loop", seed);
        const csr = g.exportCSR();
        const wantTypes = [].concat(...adj.map(a => a.map(e => e.t)));
        note(wantTypes.some(t => t) ? J(csr.types) === J(wantTypes) : !("types" in csr), "exportCSR types", seed);
        for (let q = 0; q < 6; q++) {
            const mask = pickMask(r), s = Math.floor(r() * spec.n), d = Math.floor(r() * spec.n);
            const fg = build(spec, t => admits(mask, t));
            const bfs = g.bfs(s, { types: mask }), dfs = g.dfs(s, { types: mask });
            note(J(bfs) === J(refBfs(adj, s, mask)) && J(bfs) === J(fg.bfs(s)), "bfs mask " + mask, seed, J(bfs) + " vs " + J(refBfs(adj, s, mask)));
            note(J(dfs) === J(refDfs(adj, s, mask)) && J(dfs) === J(fg.dfs(s)), "dfs mask " + mask, seed, J(dfs) + " vs " + J(refDfs(adj, s, mask)));
            const vb = [], vd = [];
            g.bfs(s, { types: mask, visit: x => { vb.push(x); } });
            g.dfs(s, { types: mask, visit: x => { vd.push(x); } });
            note(J(vb) === J(bfs) && J(vd) === J(dfs), "visitor forms", seed);
            note(J(g.shortestPath(s, d, { types: mask })) === J(fg.shortestPath(s, d)), "shortestPath mask " + mask, seed);
            const k = Math.floor(r() * 4);
            note(J(g.neighborhood(s, k, { types: mask })) === J(refHop(adj, s, k, mask)), "neighborhood", seed);
            const hops = Array.from({ length: Math.floor(r() * 4) }, () => pickMask(r));
            const srcs = Array.from({ length: 1 + Math.floor(r() * 3) }, () => Math.floor(r() * spec.n));
            note(J(g.pathEndpoints(srcs, hops)) === J(refEnds(adj, srcs, hops)), "pathEndpoints", seed);
            const want = refPaths(adj, srcs, hops);
            if (want.length <= 5000) {
                const rows = want.length, nodes = new Int32Array(rows * (hops.length + 1)), types = new Uint8Array(rows * hops.length);
                const total = g.pathsInto(srcs, hops, nodes, types);
                const got = [];
                for (let i = 0; i < Math.min(total, rows); i++)
                    got.push({ nodes: Array.from(nodes.slice(i * (hops.length + 1), (i + 1) * (hops.length + 1))), types: Array.from(types.slice(i * hops.length, (i + 1) * hops.length)) });
                note(total === want.length && J(got) === J(want), "pathsInto", seed, total + " vs " + want.length);
            }
            note(J(g.bfs(s, { types: -1 })) === J(g.bfs(s)) && J(g.shortestPath(s, d, { types: -1 })) === J(g.shortestPath(s, d)), "all-types == unmasked", seed);
        }
    }
    ok(bad === 0, "300 random multigraphs x 6 queries agree (" + checks + " checks)", bad + " mismatches");
});

__kitS.section("pathsInto capacity: fill to capacity, return the total", () => {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1, undefined, 1).addEdge(0, 1, undefined, 2).addEdge(1, 2, undefined, 1).addEdge(1, 0, undefined, 3).addEdge(0, 2, undefined, 1);
    const hops = [-1, -1];
    const want = refPaths(refAdj({ n: 3, directed: true, edges: [[0, 1, 1, 1], [0, 1, 1, 2], [1, 2, 1, 1], [1, 0, 1, 3], [0, 2, 1, 1]] }), [0], hops);
    ok(want.length === 4, "reference count", String(want.length));
    for (const rows of [0, 1, want.length - 1, want.length, want.length + 2]) {
        const nodes = new Int32Array(rows * 3).fill(-9), types = new Uint8Array(rows * 2).fill(99);
        const n = g.pathsInto(0, hops, nodes, types);
        const k = Math.min(rows, want.length);
        ok(n === want.length, "rows " + rows + " returns the total", String(n));
        eqJ(Array.from(nodes.slice(0, k * 3)), [].concat(...want.slice(0, k).map(x => x.nodes)), "rows " + rows + " node rows");
        eqJ(Array.from(types.slice(0, k * 2)), [].concat(...want.slice(0, k).map(x => x.types)), "rows " + rows + " type rows");
        ok(nodes.slice(k * 3).every(x => x === -9) && types.slice(k * 2).every(x => x === 99), "rows " + rows + " leaves the rest untouched");
    }
    const nodesOnly = new Int32Array(12);
    ok(g.pathsInto(0, hops, nodesOnly) === 4 && nodesOnly[3] === 0 && nodesOnly[4] === 1, "outTypes is optional");
    ok(g.pathsInto([0, 0], [], new Int32Array(1)) === 2, "no hops: one row per listed source");
    ok(g.pathsInto(2, [-1], new Int32Array(0)) === 0, "a sink source has no walks");
});

__kitS.section("persist: old records byte-identical, typed records round-trip", () => {
    const OLD = {
        u: "44594e5301000e00000000001b000000000000000000ffffffff060000000302000403000403030202040100010400ed1f43ad",
        d: "44594e5301000e000000000016000000000000000100ffffffff06000000020200010402040401000000d24de0ce",
        empty: "44594e5301000e00000000000a000000000000000000ffffffff0000000036954195",
    };
    for (const [k, h] of Object.entries(OLD)) {
        const g = Graph.deserialize(unhex(h));
        eqJ(hex(g.serialize()), h, "old record " + k + " re-serializes byte-identically");
        ok(!("types" in g.exportCSR()), "old record " + k + " is untyped");
    }
    const u = Graph.deserialize(unhex(OLD.u));
    eqJ(u.bfs(0, { types: 1 }), u.bfs(0), "old records walk as type 0");
    for (let seed = 1; seed <= 60; seed++) {
        const spec = randomGraph(seed * 31);
        const g = build(spec);
        const bytes = g.serialize();
        const h = Graph.deserialize(bytes);
        ok(J(h.exportCSR()) === J(g.exportCSR()) && h.edgeCount === g.edgeCount && hex(h.serialize()) === hex(bytes), "typed round trip seed " + seed);
        const typed = spec.edges.some(e => e[3]);
        ok(hex(bytes.subarray(22, 26)) === (typed ? "feffffff" : "ffffffff"), "marker seed " + seed + (typed ? " typed" : " untyped"));
    }
});

__kitS.section("persist: malformed typed records are rejected", () => {
    const seal = (b) => { const v = new DataView(b.buffer, b.byteOffset, b.byteLength); v.setUint32(b.length - 4, CRC32C(b.subarray(0, b.length - 4)) >>> 0, true); return b; };
    const g = new Graph({ directed: true });
    g.addEdge(0, 1, undefined, 5);
    const good = g.serialize();
    eqJ(hex(good.subarray(20, good.length - 4)), "0100feffffff0200000001020500", "typed payload layout: flags, marker, count, deg, zigzag delta, type, deg");
    const bad = seal(good.slice());
    bad[bad.length - 6] = 32;
    throws(() => Graph.deserialize(seal(bad)), TypeError, /edge type 32/, "type 32 rejected");
    const big = good.slice(); big[big.length - 6] = 255;
    throws(() => Graph.deserialize(seal(big)), TypeError, /edge type 255/, "type 255 rejected");
    const cut = new Uint8Array(good.length - 1);
    cut.set(good.subarray(0, good.length - 6)); cut.set(good.subarray(good.length - 5), good.length - 6);
    new DataView(cut.buffer).setUint32(12, cut.length - 24, true);
    throws(() => Graph.deserialize(seal(cut)), Error, null, "typed record missing its type byte");
});

__kitS.section("validation: types, masks, options", () => {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1, undefined, 3).addEdge(1, 2);
    throws(() => g.addEdge(0, 1, undefined, 32), RangeError, /\[0, 31\]/, "type 32");
    throws(() => g.addEdge(0, 1, undefined, -1), RangeError, /\[0, 31\]/, "type -1");
    throws(() => g.addEdge(0, 1, undefined, 1.5), RangeError, /\[0, 31\]/, "type 1.5");
    throws(() => g.addEdge(0, 1, undefined, NaN), RangeError, /\[0, 31\]/, "type NaN");
    throws(() => g.addEdge(0, 1, undefined, "1"), TypeError, /must be a number/, "type string");
    ok(g.edgeCount === 2, "refused edges are not added");
    throws(() => g.bfs(0, { types: 2 ** 32 }), RangeError, /type mask/, "mask 2^32");
    throws(() => g.bfs(0, { types: -(2 ** 31) - 1 }), RangeError, /type mask/, "mask below -2^31");
    throws(() => g.bfs(0, { types: 0.5 }), RangeError, /type mask/, "fractional mask");
    throws(() => g.bfs(0, { types: NaN }), RangeError, /type mask/, "NaN mask");
    throws(() => g.dfs(0, { types: "8" }), TypeError, /type mask must be a number/, "string mask");
    throws(() => g.bfs(0, { types: 1n }), TypeError, /type mask must be a number/, "bigint mask");
    eqJ(g.bfs(0, { types: -(2 ** 31) }), [0], "mask -2^31 is bit 31 only");
    throws(() => g.bfs(0, { types: 1, typo: 1 }), TypeError, /typo/, "unknown walk option");
    throws(() => g.bfs(0, {}), TypeError, /visit and\/or types/, "empty walk options");
    throws(() => g.bfs(0, { visit: 1, types: 1 }), TypeError, /visit must be a function/, "bad visit with types");
    throws(() => g.shortestPath(0, 1, { type: 1 }), TypeError, /type/, "shortestPath unknown option");
    throws(() => g.shortestPath(0, 1, 5), TypeError, /options must be an object/, "shortestPath non-object options");
    throws(() => g.neighborhood(0, -1), RangeError, /k must be/, "k -1");
    throws(() => g.neighborhood(0, 1.5), RangeError, /k must be/, "k 1.5");
    throws(() => g.neighborhood(0, "2"), TypeError, /k must be a number/, "k string");
    throws(() => g.neighborhood(9, 1), RangeError, /source/, "neighborhood source out of range");
    throws(() => g.neighborhood(0, 1, { types: 1, k: 2 }), TypeError, /"k"|k/, "neighborhood unknown option");
    throws(() => g.pathEndpoints(7, [1]), RangeError, /source node 7/, "pathEndpoints source out of range");
    throws(() => g.pathEndpoints([0, 9], [1]), RangeError, /source node 9/, "pathEndpoints array source out of range");
    throws(() => g.pathEndpoints(["0"], [1]), TypeError, /must be a node id/, "string source");
    throws(() => g.pathEndpoints(0, 8), TypeError, /array of type masks/, "hops not an array");
    throws(() => g.pathEndpoints(0, ["1"]), TypeError, /type mask must be a number/, "string hop mask");
    throws(() => g.pathEndpoints(0, { length: 1025 }), RangeError, /at most 1024/, "too many hops");
    ok(g.pathEndpoints(0, new Array(1024).fill(0)).length === 0, "1024 hops accepted");
    eqJ(g.pathEndpoints(0, Uint32Array.of(1 << 3, 1)), [2], "typed-array hops");
    const o = () => new Int32Array(6);
    throws(() => g.pathsInto(0, [-1], new Int32Array(5)), TypeError, /multiple of hops \+ 1/, "outNodes length");
    throws(() => g.pathsInto(0, [-1], new Uint32Array(6)), TypeError, /outNodes must be an Int32Array/, "outNodes type");
    throws(() => g.pathsInto(0, [-1], o(), new Uint8Array(2)), TypeError, /outTypes length/, "outTypes length");
    throws(() => g.pathsInto(0, [-1], o(), new Int8Array(3)), TypeError, /outTypes must be a Uint8Array/, "outTypes type");
    const sh = new ArrayBuffer(32);
    throws(() => g.pathsInto(0, [-1], new Int32Array(sh, 0, 6), new Uint8Array(sh, 20, 3)), TypeError, /share memory/, "overlapping outs");
    ok(g.pathsInto(0, [-1], new Int32Array(sh, 0, 6), new Uint8Array(sh, 24, 3)) === 1, "disjoint views of one buffer");
    const det = o(); det.buffer.transfer();
    throws(() => g.pathsInto(0, [-1], det), TypeError, /detached/, "detached outNodes");
    const im = new Int32Array(new ArrayBuffer(24).transferToImmutable());
    throws(() => g.pathsInto(0, [-1], im), TypeError, null, "immutable outNodes");
    const late = o();
    const hops = { length: 1, get 0() { late.buffer.transfer(); return -1; } };
    throws(() => g.pathsInto(0, hops, late), TypeError, /detached/, "hops are read before the views");
    const rab = new ArrayBuffer(24, { maxByteLength: 48 });
    const fixed = new Int32Array(rab, 0, 6); rab.resize(8);
    throws(() => g.pathsInto(0, [-1], fixed), TypeError, /out of bounds/, "fixed view past a shrunk buffer");
});

__kitS.section("Graph.fromEdges validation", () => {
    const a = Int32Array.of(0, 1), b = Int32Array.of(1, 2);
    const f = Graph.fromEdges(a, b, { directed: true, nodes: 6 });
    ok(f.nodeCount === 6 && f.edgeCount === 2 && f.hasEdge(1, 2) && !f.hasEdge(2, 1), "nodes and direction");
    ok(Graph.fromEdges(new Int32Array(0), new Int32Array(0)).nodeCount === 0, "empty");
    ok(Graph.fromEdges(a, b, { weighted: true, weights: Float64Array.of(2, 3) }).exportCSR().weights[0] === 2, "weights");
    throws(() => Graph.fromEdges(a, Int32Array.of(1)), TypeError, /same length/, "unequal lengths");
    throws(() => Graph.fromEdges(a, b, { types: Uint8Array.of(1) }), TypeError, /same length/, "types length");
    throws(() => Graph.fromEdges(a, b, { weights: Float64Array.of(1, 2) }), TypeError, /weighted: true/, "weights without weighted");
    throws(() => Graph.fromEdges(a, b, { types: Uint8Array.of(1, 32) }), RangeError, /types\[1\] = 32/, "type 32");
    throws(() => Graph.fromEdges(Int32Array.of(0, -1), b), RangeError, /node -1/, "negative id");
    throws(() => Graph.fromEdges(a, Int32Array.of(1, 1 << 26)), RangeError, /2\^26/, "id past 2^26");
    throws(() => Graph.fromEdges(a, b, { weighted: true, weights: Float64Array.of(1, NaN) }), RangeError, /not finite/, "NaN weight");
    throws(() => Graph.fromEdges(a, b, { directed: "yes" }), TypeError, /directed must be a boolean/, "non-boolean directed");
    throws(() => Graph.fromEdges(a, b, { nodez: 3 }), TypeError, /nodez/, "unknown option");
    throws(() => Graph.fromEdges(a, b, { nodes: 2 ** 26 + 1 }), RangeError, /opts.nodes/, "nodes past 2^26");
    throws(() => Graph.fromEdges(a, b, { nodes: "3" }), TypeError, /opts.nodes must be a number/, "string nodes");
    throws(() => Graph.fromEdges([0, 1], b), TypeError, /src must be an Int32Array/, "plain array src");
    throws(() => Graph.fromEdges(a, b, { types: [1, 2] }), TypeError, /opts.types must be a Uint8Array/, "plain array types");
    throws(() => Graph.fromEdges(a, b, 3), TypeError, /options must be an object/, "non-object options");
});

__kitS.section("pathsInto enumeration budget", () => {
    const g = new Graph();
    for (let i = 0; i < 200; i++) g.addEdge(0, 1, undefined, i % 32);
    throws(() => g.pathsInto(0, [-1, -1, -1, -1], new Int32Array(0)), RangeError, /arc-step budget/, "200^4 walks exceed 10^9 steps");
    eqJ(g.pathEndpoints(0, [-1, -1, -1, -1]), [0], "pathEndpoints stays linear on the same pattern");
});

__kitS.end();
print("");
print("test_structures_graph_typed: " + (fail ? "FAILURES: " + fail + " / " : "all ") + (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_structures_graph_typed: " + fail + " of " + (pass + fail) + " assertions FAILED");
