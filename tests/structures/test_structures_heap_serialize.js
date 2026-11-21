// covers: dyna:structures.Heap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_heap_serialize");
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

__kitS.section("max-heap order restored", () => {
    const h = new Heap((a, b) => b - a);
    [1, 5, 3].forEach(v => h.push(v));
    const bytes = h.serialize();
    const back = Heap.deserialize(bytes, (a, b) => b - a);
    eq([back.pop(), back.pop(), back.pop()], [5, 3, 1], "max-heap order restored");
    const nat = Heap.deserialize(bytes);
    eq([nat.pop(), nat.pop(), nat.pop()], [1, 3, 5],
       "the same bytes decoded naturally are a min-heap");
    throws(() => Heap.deserialize(bytes, 42), TypeError,
           "decoding a Heap with a non-function comparator");
});
__kitS.end();

print("test_structures_serialize: all " + n + " assertions passed");
