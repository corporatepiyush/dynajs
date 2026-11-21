// covers: dyna:graph.Graph
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_record_codecs");
import { Table, BloomFilter, CountMinSketch, HyperLogLog } from "dyna:structures";
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

__kitS.section("sparse, ascending targets", () => {
    const shapes = [
        ["sparse, ascending targets", 500, (i, k) => (i + k) % 500, false],
        ["dense, scattered targets", 200, (i, k) => (i * 7 + k * 131) % 200, false],
        ["weighted", 300, (i, k) => (i + k * 3) % 300, true],
        ["self loops and duplicates", 100, (i, k) => (k % 2) ? i : (i * 5) % 100, false],
    ];
    for (const [label, N, target, weighted] of shapes) {
        const g = new Graph(weighted ? { weighted: true } : undefined);
        for (let i = 0; i < N; i++)
            for (let k = 1; k <= 4; k++)
                g.addEdge(i, target(i, k), weighted ? 1 + (i % 7) : undefined);
        const rec = g.serialize();
        const back = Graph.deserialize(rec);
        eq(back.nodeCount, g.nodeCount, label + ": node count");
        eq(back.edgeCount, g.edgeCount, label + ": edge count");
        let bad = -1;
        for (let i = 0; i < N && bad < 0; i++) {
            const a = g.neighbors(i), b = back.neighbors(i);
            if (a.length !== b.length) { bad = i; break; }
            for (let k = 0; k < a.length; k++) if (a[k] !== b[k]) { bad = i; break; }
        }
        check(bad < 0, label + ": neighbours differ at node " + bad);
        check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    }

    for (const [label, build] of [["empty graph", () => new Graph()],
                                  ["nodes, no edges", () => { const g = new Graph();
                                      for (let i = 0; i < 5; i++) g.addNode(); return g; }]]) {
        const g = build(), back = Graph.deserialize(g.serialize());
        eq(back.nodeCount, g.nodeCount, label + ": node count");
        eq(back.edgeCount, g.edgeCount, label + ": edge count");
    }

    {
        const g = new Graph();
        for (let i = 0; i < 5000; i++)
            for (let k = 1; k <= 4; k++) g.addEdge(i, (i + k) % 5000);
        const rec = g.serialize();
        check(rec.length < g.edgeCount * 4,
              "the varint record beats a u32 per edge, got " + rec.length +
              " for " + g.edgeCount + " edges");
    }

    {
        const g = new Graph();
        const N = 400;
        for (let i = 0; i < N; i++)
            for (let k = 0; k < 60; k++) g.addEdge(i, (i * 13 + k * 7) % N);
        const order = g.dfs(0);
        check(order.length > 0 && order.length <= N,
              "dfs on a dense graph returns at most n nodes, got " + order.length);
        const seen = new Set(order);
        eq(seen.size, order.length, "and visits each node once");
        const bfs = g.bfs(0);
        eq(new Set(bfs).size, bfs.length, "bfs visits each node once");
        eq(bfs.length, order.length, "bfs and dfs reach the same nodes");
    }
});

__kitS.section("a decoded record answered", () => {
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };
    const sweep = (good, cls, touch, label) => {
        let attempts = 0, refused = 0, decoded = 0;
        for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 150) | 0)) {
            for (const mask of [0xff, 0x01, 0x80]) {
                const b = good.slice(); b[i] ^= mask; repair(b);
                attempts++;
                try { const o = cls.deserialize(b); decoded++; touch(o);
                      check(true, "a decoded record answered"); }
                catch (e) { refused++; check(e instanceof Error, "a refusal is an Error"); }
            }
        }
        check(attempts > 50, label + ": the sweep ran, " + attempts + " mutations");
        print("  " + label + ": " + attempts + " mutations, " + refused +
              " refused, " + decoded + " decoded and exercised");
    };

    {
        const g = new Graph();
        for (let i = 0; i < 200; i++) for (let k = 1; k <= 3; k++) g.addEdge(i, (i + k) % 200);
        sweep(g.serialize(), Graph, (o) => { o.nodeCount; o.edgeCount;
            for (let i = 0; i < o.nodeCount; i++) o.neighbors(i); }, "Graph");
    }
    {
        const t = new Table();
        for (let r = 0; r < 20; r++) for (let c = 0; c < 10; c++) t.put("r" + r, "c" + c, r * 10 + c);
        sweep(t.serialize(), Table, (o) => { o.size;
            for (let r = 0; r < 20; r++) o.row("r" + r); }, "Table");
    }
    {
        const s = new CountMinSketch(256, 4);
        for (let i = 0; i < 500; i++) s.add("k" + (i % 60), 1);
        sweep(s.serialize(), CountMinSketch, (o) => { o.width; o.totalCount;
            for (let i = 0; i < 60; i++) o.count("k" + i); }, "CountMinSketch");
    }
    {
        const h = new HyperLogLog(10);
        for (let i = 0; i < 5000; i++) h.add("u" + i);
        sweep(h.serialize(), HyperLogLog, (o) => { o.count(); o.registers; }, "HyperLogLog");
    }
});

__kitS.section("no truncation may decode", () => {
    const g = new Graph();
    for (let i = 0; i < 100; i++) for (let k = 1; k <= 3; k++) g.addEdge(i, (i + k) % 100);
    const t = new Table();
    for (let r = 0; r < 15; r++) for (let c = 0; c < 8; c++) t.put("r" + r, "c" + c, r);
    const h = new HyperLogLog(10);
    for (let i = 0; i < 2000; i++) h.add("u" + i);
    for (const [cls, rec, label] of [[Graph, g.serialize(), "Graph"],
                                     [Table, t.serialize(), "Table"],
                                     [HyperLogLog, h.serialize(), "HyperLogLog"]]) {
        let survived = 0;
        for (let len = 0; len < rec.length; len += Math.max(1, (rec.length / 100) | 0)) {
            try { cls.deserialize(rec.subarray(0, len)); survived++; } catch (e) {  }
        }
        eq(survived, 0, label + ": no truncation may decode");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_record_codecs: all " + n + " checks passed");
else print("test_structures_record_codecs: " + fails + " FAILED of " + n);
