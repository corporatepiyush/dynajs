// timeout: 300
// covers: dyna:graph.Graph
// Graph#topicPageRank: batched topic-sensitive personalized PageRank over typed, masked and type-weighted
// arcs. Oracles: values derived by hand on tiny graphs (fixed points), a brute-force dense power iteration
// in JS (networkx.pagerank semantics: start 1/n, dangling mass to the personalization, stop at L1 < n*tol),
// Graph#pageRank with a personalization vector (unmasked) and pageRank on a weighted graph rebuilt from
// the admitted arcs with weight * typeWeight (masked/weighted). Batched rows are bit-identical to
// one-topic calls. Also: the X4 carry-over, forged undirected typed records whose arc pairs disagree.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_topic_pagerank");
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const close = (a, b, eps) => a.length === b.length && Array.from(a).every((x, i) => Math.abs(x - b[i]) <= eps);
const same = (a, b) => a.length === b.length && Array.from(a).every((x, i) => Object.is(x, b[i]));
async function rejects(p, Ctor, re, w) {
    try { await (typeof p === "function" ? p() : p); ok(false, w, "no rejection"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
}

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

function arcsOf(spec, mask, tw) {
    const arcs = [];
    for (const [u, v, w, t] of spec.edges) {
        if (!(((mask >>> 0) >>> t) & 1)) continue;
        const ew = (spec.weighted ? w : 1) * (tw ? tw[t] : 1);
        arcs.push([u, v, ew]);
        if (!spec.directed && u !== v) arcs.push([v, u, ew]);
    }
    return arcs;
}
function persOf(n, seeds, weights) {
    const p = new Array(n).fill(0);
    const s = (weights || seeds.map(() => 1)).reduce((a, b) => a + b, 0);
    seeds.forEach((x, i) => { p[x] += (weights ? weights[i] : 1) / s; });
    return p;
}
function refPPR(n, arcs, p, alpha, tol, maxIter) {
    const outw = new Array(n).fill(0);
    for (const [u, , w] of arcs) outw[u] += w;
    let x = new Array(n).fill(1 / n);
    const errs = [];
    for (let it = 0; it < maxIter; it++) {
        let dang = 0;
        for (let u = 0; u < n; u++) if (!(outw[u] > 0)) dang += x[u];
        const pull = new Array(n).fill(0);
        for (const [u, v, w] of arcs) if (outw[u] > 0) pull[v] += x[u] * w / outw[u];
        const xn = pull.map((q, v) => alpha * q + (alpha * dang + 1 - alpha) * p[v]);
        const err = xn.reduce((a, y, v) => a + Math.abs(y - x[v]), 0);
        errs.push(err);
        x = xn;
        if (err < n * tol) return { x, k: it + 1, errs };
    }
    return { x: null, k: maxIter, errs };
}
function randomSpec(seed) {
    const r = rng(seed);
    const n = 1 + Math.floor(r() * 20), directed = r() < 0.6, weighted = r() < 0.5;
    const edges = [];
    const m = Math.floor(r() * 60);
    for (let i = 0; i < m; i++) {
        const u = Math.floor(r() * n), v = r() < 0.1 ? u : Math.floor(r() * n);
        edges.push([u, v, 0.5 + Math.floor(r() * 8), Math.floor(r() * 4) * (r() < 0.5 ? 1 : 7)]);
        if (r() < 0.15) edges.push([u, v, 1 + Math.floor(r() * 3), (edges[edges.length - 1][3] + 1) % 32]);
    }
    return { n, directed, weighted, edges, r };
}
function buildG(spec) {
    const g = new Graph({ directed: spec.directed, weighted: spec.weighted });
    for (let i = 0; i < spec.n; i++) g.addNode();
    for (const [u, v, w, t] of spec.edges) g.addEdge(u, v, spec.weighted ? w : undefined, t);
    return g;
}

print("== Graph#topicPageRank ==");

await __kitS.section("hand-derived fixed points", async () => {
    const a = 0.85, T = { tolerance: 1e-15, maxIterations: 10000 };
    const c2 = new Graph({ directed: true });
    c2.addEdge(0, 1).addEdge(1, 0);
    const r = await c2.topicPageRank([[0], [1]], T);
    ok(close(r, [1 / (1 + a), a / (1 + a), a / (1 + a), 1 / (1 + a)], 1e-10), "2-cycle seeded at 0 / at 1: 1/(1+a), a/(1+a)", Array.from(r).join());
    const r9 = await c2.topicPageRank([[0]], { ...T, damping: 0.5 });
    ok(close(r9, [2 / 3, 1 / 3], 1e-10), "2-cycle damping 0.5: 2/3, 1/3", Array.from(r9).join());
    const e3 = new Graph();
    e3.addNode(); e3.addNode(); e3.addNode();
    ok(same(await e3.topicPageRank([{ seeds: [0, 2], weights: [1, 3] }]), [0.25, 0, 0.75]), "edgeless graph: the normalized seed vector");
    const typed = new Graph({ directed: true });
    typed.addEdge(0, 1, undefined, 4).addEdge(1, 2, undefined, 4).addEdge(2, 0, undefined, 4);
    ok(same(await typed.topicPageRank([{ seeds: [1, 1, 2], weights: [1, 1, 2] }], { types: 0 }), [0, 0.5, 0.5]), "mask 0: every node dangling, row = seed vector (repeated seeds add)");
    ok(same(await typed.topicPageRank([[1]], { types: 1 << 3 }), [0, 1, 0]), "mask without the edges' type: same");
    const loop = new Graph({ directed: true });
    loop.addEdge(0, 0, undefined, 9);
    ok(same(await loop.topicPageRank([[0]]), [1]), "lone self loop: 1");
    const star = new Graph({ directed: true });
    star.addEdge(0, 1).addEdge(0, 2);
    const rs = await star.topicPageRank([[0]], T);
    const s0 = (1 - a) / (1 - a * (a)), leaf = a * s0 / 2;
    ok(close(rs, [s0, leaf, leaf], 1e-10), "star seeded at hub: x0 = (1-a)/(1-a^2), leaves a*x0/2 (dangling leaves feed the hub)", Array.from(rs).join() + " vs " + [s0, leaf, leaf]);
    ok((await new Graph().topicPageRank([])).length === 0, "no topics on an empty graph: empty result");
    ok((await c2.topicPageRank([])).length === 0, "no topics: empty result");
});

await __kitS.section("differential: dense JS power iteration, pageRank, batched == single", async () => {
    let bad = 0, cmp = 0, iterChecks = 0;
    const note = (c, w, seed, d) => { cmp++; if (!c && bad++ < 6) print("  seed " + seed + " " + w + (d ? " " + d : "")); };
    for (let seed = 1; seed <= 150; seed++) {
        const spec = randomSpec(seed), r = spec.r, g = buildG(spec), n = spec.n;
        const k = r();
        const mask = k < 0.3 ? -1 : k < 0.4 ? 0 : Math.floor(r() * 4294967296) | 1;
        const tw = r() < 0.5 ? null : Float64Array.from({ length: 32 }, () => (r() < 0.2 ? 0 : Math.floor(r() * 5) / 2));
        const alpha = [0.85, 0.5, 0.99, 0][Math.floor(r() * 4)];
        const topics = [];
        for (let t = 0, nt = 1 + Math.floor(r() * 5); t < nt; t++) {
            const ns = 1 + Math.floor(r() * 3), seeds = Array.from({ length: ns }, () => Math.floor(r() * n));
            topics.push(r() < 0.5 ? seeds : { seeds: Int32Array.from(seeds), weights: seeds.map(() => 0.5 + Math.floor(r() * 4)) });
        }
        const opts = { damping: alpha, tolerance: 1e-10, maxIterations: 5000, types: mask, ...(tw ? { typeWeights: tw } : {}) };
        const res = await g.topicPageRank(topics, opts);
        note(res.length === topics.length * n, "result shape", seed);
        const arcs = arcsOf(spec, mask, tw);
        for (let t = 0; t < topics.length; t++) {
            const tp = topics[t], seeds = Array.from(tp.seeds || tp), weights = tp.weights || null;
            const p = persOf(n, seeds, weights);
            const row = res.subarray(t * n, (t + 1) * n);
            const ref = refPPR(n, arcs, p, alpha, 1e-10, 5000);
            note(ref.x && close(row, ref.x, 1e-9), "vs dense JS topic " + t, seed, ref.x ? Array.from(row).slice(0, 4) + " vs " + ref.x.slice(0, 4) : "ref no convergence");
            const single = await g.topicPageRank([tp], opts);
            note(same(single, row), "batched row == single call topic " + t, seed);
            const wg = new Graph({ directed: spec.directed, weighted: true });
            for (let i = 0; i < n; i++) wg.addNode();
            for (const [u, v, w, ty] of spec.edges) if ((((mask >>> 0) >>> ty) & 1)) wg.addEdge(u, v, (spec.weighted ? w : 1) * (tw ? tw[ty] : 1));
            const pr = await wg.pageRank({ personalization: p, damping: alpha, tolerance: 1e-10, maxIterations: 5000 });
            note(close(row, pr, 1e-9), "vs pageRank on the filtered weighted graph topic " + t, seed);
            if (ref.x && ref.k > 1 && ref.errs[ref.k - 1] < n * 1e-10 * 0.99 && ref.errs[ref.k - 2] > n * 1e-10 * 1.01) {
                iterChecks++;
                const at = await g.topicPageRank([tp], { ...opts, maxIterations: ref.k });
                note(same(at, row), "maxIterations = k converges", seed);
                let msg = "";
                try { await g.topicPageRank([tp], { ...opts, maxIterations: ref.k - 1 }); } catch (e) { msg = e.message; }
                note(msg === "topicPageRank: topic 0: no convergence within " + (ref.k - 1) + " iterations", "maxIterations = k - 1 rejects", seed, msg);
            }
        }
        if (mask === -1 && !tw) {
            const p = persOf(n, Array.from(topics[0].seeds || topics[0]), topics[0].weights || null);
            const pr = await g.pageRank({ personalization: p, damping: alpha, tolerance: 1e-10, maxIterations: 5000 });
            note(close(res.subarray(0, n), pr, 1e-12), "unmasked == Graph#pageRank(personalization)", seed);
        }
    }
    ok(bad === 0, "150 random typed multigraphs agree (" + cmp + " comparisons, " + iterChecks + " iteration-count checks)", bad + " mismatches");
    ok(iterChecks > 50, "enough iteration-count checks", String(iterChecks));
});

await __kitS.section("type weights and masks", async () => {
    const g = new Graph({ directed: true, weighted: true });
    g.addEdge(0, 1, 2, 1).addEdge(0, 2, 1, 2).addEdge(1, 0, 1, 1).addEdge(2, 0, 1, 2).addEdge(1, 2, 3, 5).addEdge(2, 2, 1, 5);
    const tw = new Float64Array(32).fill(1);
    tw[5] = 0;
    const a = await g.topicPageRank([[0], [1, 2]], { typeWeights: tw });
    const b = await g.topicPageRank([[0], [1, 2]], { types: ~(1 << 5) });
    ok(same(a, b), "typeWeights[t] = 0 equals clearing bit t");
    const ones = await g.topicPageRank([[0]], { typeWeights: Array(32).fill(1) });
    ok(same(ones, await g.topicPageRank([[0]])), "all-ones typeWeights equals none");
    ok(same(await g.topicPageRank([[0]], { types: -1 }), await g.topicPageRank([[0]])), "all-types mask equals none");
    const twice = new Float64Array(32).fill(2);
    ok(close(await g.topicPageRank([[0]], { typeWeights: twice }), await g.topicPageRank([[0]]), 1e-12), "uniform scaling of type weights does not change ranks");
    const tilt = new Float64Array(32).fill(1);
    tilt[1] = 10;
    const tr = await g.topicPageRank([[0]], { typeWeights: tilt }), base = await g.topicPageRank([[0]]);
    ok(tr[1] > base[1], "up-weighting type 1 raises the type-1 neighbour");
});

await __kitS.section("validation", async () => {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1).addEdge(1, 2);
    await rejects(() => g.topicPageRank(0), TypeError, /topics must be an array/, "topics not an array");
    await rejects(() => g.topicPageRank([5]), TypeError, /topics\[0\] must be/, "topic is a number");
    await rejects(() => g.topicPageRank([{ seed: [0] }]), TypeError, /seed/, "unknown topic key");
    await rejects(() => g.topicPageRank([{ weights: [1] }]), TypeError, /needs seeds/, "topic without seeds");
    await rejects(() => g.topicPageRank([[]]), RangeError, /no seeds/, "empty seeds");
    await rejects(() => g.topicPageRank([[3]]), RangeError, /seed node 3 out of range/, "seed out of range");
    await rejects(() => g.topicPageRank([[-1]]), RangeError, /seeds\[0\]/, "negative seed");
    await rejects(() => g.topicPageRank([[0.5]]), RangeError, /seeds\[0\]/, "fractional seed");
    await rejects(() => g.topicPageRank([Float64Array.of(0)]), TypeError, /must hold integers/, "Float64Array seeds");
    await rejects(() => g.topicPageRank([{ seeds: [0, 1], weights: [1] }]), RangeError, /weights has 1 entries/, "weights length");
    await rejects(() => g.topicPageRank([{ seeds: [0], weights: [-1] }]), RangeError, /finite and >= 0/, "negative seed weight");
    await rejects(() => g.topicPageRank([{ seeds: [0], weights: [NaN] }]), RangeError, /finite and >= 0/, "NaN seed weight");
    await rejects(() => g.topicPageRank([{ seeds: [0, 1], weights: [0, 0] }]), RangeError, /positive finite sum/, "zero weight sum");
    await rejects(() => g.topicPageRank([[0]], { typeWeights: new Float64Array(31) }), RangeError, /31 entries, expected 32/, "typeWeights length");
    await rejects(() => g.topicPageRank([[0]], { typeWeights: Int32Array.from({ length: 32 }) }), TypeError, /Float64Array/, "typeWeights Int32Array");
    const neg = new Float64Array(32); neg[7] = -1;
    await rejects(() => g.topicPageRank([[0]], { typeWeights: neg }), RangeError, /typeWeights\[7\]/, "negative type weight");
    const inf = new Float64Array(32); inf[0] = Infinity;
    await rejects(() => g.topicPageRank([[0]], { typeWeights: inf }), RangeError, /typeWeights\[0\]/, "infinite type weight");
    await rejects(() => g.topicPageRank([[0]], { dampening: 0.5 }), TypeError, /dampening/, "unknown option");
    await rejects(() => g.topicPageRank([[0]], { damping: 1.5 }), RangeError, /damping/, "damping out of range");
    await rejects(() => g.topicPageRank([[0]], { maxIterations: 0 }), RangeError, /maxIterations/, "maxIterations 0");
    await rejects(() => g.topicPageRank([[0]], { types: "1" }), TypeError, /type mask/, "string mask");
    await rejects(() => g.topicPageRank([[0]], 3), TypeError, /options must be an object/, "non-object options");
    const ng = new Graph({ directed: true, weighted: true });
    ng.addEdge(0, 1, -1);
    await rejects(() => ng.topicPageRank([[0]]), RangeError, /negative edge weight/, "negative edge weight");
    const big = Graph.fromEdges(new Int32Array(0), new Int32Array(0), { nodes: 1 << 21 });
    await rejects(() => big.topicPageRank(Array.from({ length: 65 }, () => [0])), RangeError, /exceeds/, "topics x nodes past 2^27");
    await rejects(() => g.topicPageRank(Array.from({ length: 65537 }, () => [0])), RangeError, /at most 65536/, "too many topics");
    const huge = new Graph({ directed: true, weighted: true });
    huge.addEdge(0, 1, 1e308).addEdge(0, 2, 1e308);
    await rejects(() => huge.topicPageRank([[0]]), RangeError, /invalid input|non-finite/, "out-weight sum overflows");
});

await __kitS.section("read order: options and topics before the snapshot", async () => {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1);
    const opts = { get damping() { g.addEdge(1, 0); return 0.85; } };
    Object.defineProperty(opts, "damping", { enumerable: true });
    const r = await g.topicPageRank([[0]], opts);
    const c = new Graph({ directed: true });
    c.addEdge(0, 1).addEdge(1, 0);
    ok(same(r, await c.topicPageRank([[0]])), "an option getter's addEdge is part of the run");
    const h = new Graph({ directed: true });
    h.addEdge(0, 1);
    const want = await h.topicPageRank([[0]]);
    const p = h.topicPageRank([[0]]);
    h.addEdge(1, 0);
    ok(same(await p, want), "mutating the graph after the call does not affect the run");
    const late = { length: 1, get 0() { g.addNode(); return [g.nodeCount - 1]; } };
    await rejects(() => g.topicPageRank(late), TypeError, /topics must be an array/, "array-likes are not topics arrays");
    const grow = [{ get seeds() { g.addNode(); return [g.nodeCount - 1]; } }];
    Object.defineProperty(grow[0], "seeds", { enumerable: true });
    const rg = await g.topicPageRank(grow);
    ok(rg.length === g.nodeCount && rg[g.nodeCount - 1] > 0, "a node added by a seeds getter is in range and in the result");
});

await __kitS.section("X4 carry-over: undirected typed records must pair arc types", async () => {
    const rec = (payload) => {
        const head = [0x44, 0x59, 0x4e, 0x53, 1, 0, 0x0e, 0, 0, 0, 0, 0, payload.length, 0, 0, 0, 0, 0, 0, 0];
        const b = new Uint8Array([...head, ...payload, 0, 0, 0, 0]);
        new DataView(b.buffer).setUint32(b.length - 4, CRC32C(b.subarray(0, b.length - 4)) >>> 0, true);
        return b;
    };
    const body = (a0, a1) => [0, 0, 0xfe, 0xff, 0xff, 0xff, 2, 0, 0, 0, a0.length, ...a0.flat(), a1.length, ...a1.flat()];
    const g = new Graph();
    g.addEdge(0, 1, undefined, 1).addEdge(0, 1, undefined, 2);
    const real = g.serialize();
    ok(Array.from(real).join() === Array.from(rec(body([[2, 1], [0, 2]], [[0, 1], [0, 2]]))).join(), "the forging helper reproduces a real record");
    ok(Graph.deserialize(rec(body([[2, 1], [0, 2]], [[0, 2], [0, 1]]))).edgeCount === 2, "pairs in a different order are fine (multiset)");
    const mk = (a0, a1) => () => Graph.deserialize(rec(body(a0, a1)));
    const bad = [
        [mk([[2, 1], [0, 2]], [[0, 1], [0, 1]]), "parallel {1,2} against {1,1}"],
        [mk([[2, 3]], [[0, 4]]), "one arc's type flipped"],
        [mk([[2, 0]], [[0, 3]]), "type 0 against a nonzero type"],
        [mk([[2, 3]], [[2, 3]]), "the other end points elsewhere (no partner)"],
    ];
    for (const [fn, w] of bad) {
        try { fn(); ok(false, w, "no throw"); }
        catch (e) { ok(e instanceof TypeError && /malformed Graph record: undirected arcs do not pair/.test(e.message), w, e.message); }
    }
    const loops = new Graph();
    loops.addEdge(1, 1, undefined, 3).addEdge(0, 1, undefined, 2);
    const lb = loops.serialize();
    ok(Graph.deserialize(lb).exportCSR().types.join() === loops.exportCSR().types.join(), "self loops are exempt from pairing");
    const dg = new Graph({ directed: true });
    dg.addEdge(0, 1, undefined, 1);
    ok(Graph.deserialize(dg.serialize()).edgeCount === 1, "directed typed records are not paired");
});

__kitS.end();
print("");
print("test_structures_graph_topic_pagerank: " + (fail ? "FAILURES: " + fail + " / " : "all ") + (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_structures_graph_topic_pagerank: " + fail + " of " + (pass + fail) + " assertions FAILED");
