// covers: dyna:graph.Graph
// B1f (2026-11-26, clean break): Graph and the graph analytics namespace moved out of dyna:structures into
// their own dyna:graph module; no old import path remains. dyna:graph registers the Graph codec and installs
// serialize/deserialize/save/load itself, and src/dyna-serialize.c builds for either module, so dyna:graph
// links and persists without dyna:structures (tools/module-matrix.js --only graph).
// Mutants: codec methods not installed by dyna:graph (red), dyna:graph not registered (red), the serialize
// kit gated to structures only (module-matrix graph link red).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_serialize");
import { BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter, Trie, LRU, SortedSet, SortedMap, Heap, List, Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree, MinMaxHeap, CountMinSketch, HyperLogLog } from "dyna:structures";
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error((m || "mismatch") + ":\n  got  " + a + "\n  want " + b);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
const CASES = [
    ["BitSet", () => { const b = new BitSet(); [0, 1, 63, 64, 65, 200, 4095].forEach(i => b.set(i)); return b; },
     o => [o.count, o.toArray()]],
    ["UnionFind", () => { const u = new UnionFind(32); u.union(1, 2); u.union(3, 4); u.union(2, 4); u.union(10, 11); return u; },
     o => [o.size, o.count, [...Array(32).keys()].map(i => o.find(i))]],
    ["Deque", () => { const d = new Deque(); [1, "two", { a: 3 }, [4, 5], null, true].forEach(v => d.pushBack(v)); d.popFront(); return d; },
     o => [o.length, o.toArray()]],
    ["Fenwick", () => { const f = new Fenwick(16); f.update(0, 1.5); f.update(3, -2.25); f.update(15, 7); return f; },
     o => [o.size, [...Array(16).keys()].map(i => o.prefixSum(i))]],
    ["RingBuffer", () => { const r = new RingBuffer(5); for (let i = 0; i < 8; i++) r.push(i * 11); return r; },
     o => [o.capacity, o.length, o.full, o.toArray()]],
    ["SegTree", () => { const s = new SegTree(16, "min"); for (let i = 0; i < 16; i++) s.update(i, (i * 7) % 13); return s; },
     o => [o.size, [...Array(16).keys()].map(i => o.rangeQuery(i, 15))]],
    ["BloomFilter", () => { const b = new BloomFilter(2048, 5); for (let i = 0; i < 100; i++) b.add("k" + i); return b; },
     o => [o.bits, o.hashes, [...Array(200).keys()].map(i => o.mayContain("k" + i))]],
    ["Trie", () => { const t = new Trie(); ["", "a", "ab", "abc", "b", "zzz", "éè"].forEach(k => t.insert(k)); return t; },
     o => [o.size, o.keysWithPrefix("").sort(), o.longestPrefix("abcd")]],
    ["LRU", () => { const c = new LRU(4); ["a", "b", "c", "d"].forEach((k, i) => c.put(k, i)); c.get("a"); c.get("c"); return c; },
     o => [o.capacity, o.size, ["a", "b", "c", "d"].map(k => o.get(k))]],
    ["SortedSet", () => { const s = new SortedSet(); [5, -1, 9, 3, 0.5, 1e9].forEach(v => s.add(v)); return s; },
     o => [o.size, o.toArray(), o.floor(4), o.ceil(4)]],
    ["SortedMap", () => { const m = new SortedMap(); m.set(5, "e"); m.set(1, { deep: [1, 2] }); m.set(-3, null); return m; },
     o => [o.size, o.rangeQuery(-1e9, 1e9), o.keys()]],
    ["Heap", () => { const h = new Heap((a, b) => a - b); [5, 1, 9, 3, 7, 2].forEach(v => h.push(v)); return h; },
     o => { const r = []; while (o.size) r.push(o.pop()); return r; }, (a, b) => a - b],
    ["List", () => { const l = new List(); [1, 2, 3].forEach(v => l.pushBack(v)); l.pushFront(0); return l; },
     o => [o.length, o.toArray()]],
    ["Multiset", () => { const m = new Multiset(); m.add("a", 3); m.add("b"); m.add("é", 1000000); return m; },
     o => [o.size, o.totalSize, o.entrySet().sort()]],
    ["Multimap", () => { const m = new Multimap(); m.put("x", 1).put("x", 2).put("y", { z: 3 }).put("x", 1); return m; },
     o => [o.size, o.keyCount, o.entries()]],
    ["BiMap", () => { const b = new BiMap(); b.set("a", "1").set("b", "2").set("c", "3"); return b; },
     o => [o.size, o.entries(), o.keyOf("2")]],
    ["Table", () => { const t = new Table(); t.put("r1", "c1", 11).put("r1", "c2", [1, 2]).put("r2", "c1", { x: 1 }); return t; },
     o => [o.size, o.cells(), o.row("r1")]],
    ["RangeSet", () => { const r = new RangeSet(); r.add(1, 5).add(20, 30).add(-Infinity, -100); return r; },
     o => [o.size, o.ranges(), o.contains(3), o.contains(10)]],
    ["RangeMap", () => { const m = new RangeMap(); m.put(0, 10, "a").put(3, 5, "b").put(100, 200, { deep: 1 }); return m; },
     o => [o.size, o.entries(), o.get(4), o.get(7)]],
    ["IntervalTree", () => { const t = new IntervalTree(); t.insert(1, 5, "a").insert(4, 9, "b").insert(20, 25, "c"); return t; },
     o => [o.size, o.overlapping(0, 100).map(x => x[2]).sort(), o.at(4).map(x => x[2]).sort()]],
    ["MinMaxHeap", () => { const h = new MinMaxHeap(); [5, 1, 9, 3, 7, 2, 8].forEach(v => h.push(v, "v" + v)); return h; },
     o => { const r = []; while (o.size) { r.push(o.popMin()); if (o.size) r.push(o.popMax()); } return r; }],
    ["CountMinSketch", () => { const s = new CountMinSketch(512, 4); for (let i = 0; i < 300; i++) s.add("q" + (i % 50), i); return s; },
     o => [o.width, o.depth, o.totalCount, [...Array(50).keys()].map(i => o.count("q" + i))]],
    ["HyperLogLog", () => { const h = new HyperLogLog(10); for (let i = 0; i < 2000; i++) h.add("u" + i); return h; },
     o => [o.precision, o.registers, Math.round(o.count())]],
    ["Graph", () => { const g = new Graph({ directed: true, weighted: true });
                      g.addEdge(0, 1, 2).addEdge(1, 2, 3).addEdge(0, 2, 10).addEdge(2, 3, -1.5);
                      g.addNode();
                      return g; },
     o => [o.nodeCount, o.edgeCount, o.bellmanFord(0), o.topologicalSort(),
           [...Array(5).keys()].map(i => o.neighbors(i))]],
    ["Graph(undirected)", () => { const g = new Graph();
                                  g.addEdge(0, 1).addEdge(1, 2).addEdge(0, 1).addEdge(3, 3);
                                  return g; },
     o => [o.nodeCount, o.edgeCount, o.connectedComponents(), o.bfs(0),
           [...Array(4).keys()].map(i => o.neighbors(i))], undefined, "Graph"],
];

__kitS.section("encode returns bytes", () => {
    for (const [name, build, project, arg, cls] of CASES) {
        const original = build();
        const want = project(build());
        const bytes = original.serialize();
        const type = cls || name;
        const C = original.constructor;
        assert(bytes instanceof Uint8Array, name + " encode returns bytes");
        const back = C.deserialize(bytes, arg);
        assert(back.constructor.name === type, name + " decoded to the right class");
        eq(project(back), want, name + " round-trip");
    }
});

__kitS.section("re-encode length", () => {
    for (const [name, build, , arg] of CASES) {
        const src = build();
        const a = src.serialize();
        const b = src.constructor.deserialize(a, arg).serialize();
        assert(a.length === b.length, name + " re-encode length " + a.length + " vs " + b.length);
        let diff = -1;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { diff = i; break; }
        assert(diff < 0, name + " re-encode differs at byte " + diff);
    }
});

__kitS.section("bad magic", () => {
    const G0 = CASES[0][1]().constructor;
    const good = (CASES[0][1]().serialize());

    const bad = (mutate, why) => {
        const b = good.slice();
        mutate(b);
        throws(() => G0.deserialize(b), TypeError, why);
    };
    bad(b => { b[0] ^= 1; }, "bad magic");
    bad(b => { b[4] = 99; }, "unsupported version");
    bad(b => { b[b.length - 1] ^= 0xff; }, "corrupted CRC");
    bad(b => { b[20] ^= 0xff; }, "corrupted payload (caught by the CRC)");
    bad(b => { b[12] = 0xff; }, "payload_len that does not match the buffer");
    bad(b => { b[6] = 0xfe; b[7] = 0xff; }, "unknown type_id");

    throws(() => G0.deserialize(good.slice(0, 10)), TypeError, "truncated header");
    throws(() => G0.deserialize(new Uint8Array(0)), TypeError, "empty");
    throws(() => G0.deserialize(new Uint8Array(23)), TypeError, "all zeros");
    throws(() => G0.deserialize("not bytes"), TypeError, "wrong argument type");

    bad(b => { b[12] = 0; b[13] = 0; b[14] = 0; b[15] = 0x10; }, "absurd payload_len");

    const bigSrc = CASES[21][1]();
    const big = bigSrc.serialize();
    throws(() => bigSrc.constructor.deserialize(big.subarray(0, 40)), TypeError,
           "truncated big record");

    assert(typeof ({}).serialize === "undefined",
           "a plain object has no serialize()");
    throws(() => G0.deserialize({}), TypeError, "a plain object is not a record");
    assert(G0.deserialize(good) instanceof G0, "a good record still decodes");
});

__kitS.section("non-Error thrown from decode", () => {
    let attempts = 0, decoded = 0, threw = 0;
    for (const [name, build, , arg] of CASES) {
        const src = build();
        const C = src.constructor;
        const good = src.serialize();
        const step = good.length <= 300 ? 1 : Math.ceil(good.length / 300);
        for (let i = 0; i < good.length; i += step) {
            for (const mask of [0xff, 0x01, 0x80]) {
                const b = good.slice();
                b[i] ^= mask;
                attempts++;
                try {
                    const v = C.deserialize(b, arg);
                    decoded++;
                    if (v && typeof v.size === "number") void v.size;
                } catch (e) {
                    threw++;
                    if (!(e instanceof Error))
                        throw new Error(name + ": non-Error thrown from decode");
                }
            }
        }
    }
    assert(attempts > 2000, "mutation sweep ran (" + attempts + " cases)");
    assert(threw > attempts * 0.95,
           "the CRC rejected only " + threw + " of " + attempts + " corruptions");
    print("  mutation sweep: " + attempts + " corrupted records, " +
          threw + " rejected, " + decoded + " still decoded");
});

__kitS.section("non-Error thrown from decode (L175)", () => {
    const HEADER = 20, TRAILER = 4;
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - TRAILER + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    let attempts = 0, decoded = 0, threw = 0;
    for (const [name, build, , arg] of CASES) {
        const src = build();
        const C = src.constructor;
        const good = src.serialize();
        const last = good.length - TRAILER;
        const step = (last - HEADER) <= 200 ? 1 : Math.ceil((last - HEADER) / 200);
        for (let i = HEADER; i < last; i += step) {
            for (const mask of [0xff, 0x01, 0x40]) {
                const b = good.slice();
                b[i] ^= mask;
                repair(b);
                attempts++;
                try {
                    const v = C.deserialize(b, arg);
                    decoded++;
                    if (v) {
                        void v.size;
                        if (typeof v.toArray === "function") v.toArray();
                        if (typeof v.entries === "function") v.entries();
                        if (typeof v.ranges === "function") v.ranges();
                        if (typeof v.cells === "function") v.cells();
                        if (typeof v.neighbors === "function") {
                            const nc = v.nodeCount;
                            void v.edgeCount;
                            for (let k = 0; k < nc; k++) v.neighbors(k);
                            if (nc > 0) { v.bfs(0); v.connectedComponents(); }
                        }
                    }
                } catch (e) {
                    threw++;
                    if (!(e instanceof Error))
                        throw new Error(name + ": non-Error thrown from decode");
                }
            }
        }
    }
    assert(attempts > 2000, "checksum-repaired sweep ran (" + attempts + " cases)");
    print("  repaired sweep:  " + attempts + " forged records, " +
          threw + " rejected, " + decoded + " decoded and exercised");
    assert(decoded > 0, "the repaired sweep must actually reach the readers");
});

__kitS.section("truncation sweep ran", () => {
    let cases = 0;
    for (const [, build, , arg] of CASES) {
        const src = build();
        const C = src.constructor;
        const good = src.serialize();
        const step = Math.max(1, Math.ceil(good.length / 60));
        for (let len = 0; len < good.length; len += step) {
            cases++;
            try { C.deserialize(good.subarray(0, len), arg); }
            catch (e) {  }
        }
    }
    assert(cases > 200, "truncation sweep ran (" + cases + " cases)");
});

__kitS.section("graph record layout is where the test thinks", () => {
    const reseal = b => {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    };
    const g = new Graph({ directed: true, weighted: true });
    g.addEdge(0, 1, 2).addEdge(1, 2, 3);
    const gb = g.serialize();
    eq([gb[22], gb[26], gb[30], gb[31]], [255, 3, 1, 2],
       "graph record layout is where the test thinks");

    const outOfRange = gb.slice(); outOfRange[31] = 10; reseal(outOfRange);
    throws(() => Graph.deserialize(outOfRange), Error, "forged Graph edge past nodeCount");

    const inRange = gb.slice(); inRange[31] = 4; reseal(inRange);
    assert(Graph.deserialize(inRange).nodeCount === 3, "a legal retarget still decodes");

    const u = new Graph(); u.addEdge(0, 1);
    const ub = u.serialize();
    const odd = ub.slice(); odd[32] = 0; reseal(odd);
    throws(() => Graph.deserialize(odd), Error, "forged asymmetric undirected adjacency");
});
__kitS.end();

print("test_structures_serialize: all " + n + " assertions passed");
