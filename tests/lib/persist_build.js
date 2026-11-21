// N-scaled builders for every dyna:structures class, shared by the persist
// suites. build(n) makes an instance holding about n elements in a scrambled
// insertion order, probe(o, n) runs read queries whose answers must survive a
// save/load, mutate(o, n) changes the instance through its public API so a
// loaded copy can be shown to be live and independent of the file.
// ConsistentHash counts ring points (nodes x replicas x weight), not nodes:
// addNode rebuilds the whole ring, so n nodes would cost O(n^2 log n).
import * as S from "dyna:structures";
import * as GraphMod from "dyna:graph";

export const SIZES = [128, 1024, 10240, 51200, 102400];

const perm = (i, n) => (i * 7919 + 13) % n;
const pick = (n) => [0, 1, (n >> 1) | 0, n - 1, n + 5];
const words = ["red", "fox", "dog", "the", "jumps", "lazy", "over", "quick"];

function seqProbe(o) { return o.toArray().filter((_, i, a) => i < 8 || i >= a.length - 8); }

export const PERSIST = {
    BiMap: {
        build(n) { const x = new S.BiMap(); for (let i = 0; i < n; i++) x.set("k" + perm(i, n), "v" + i); return x; },
        probe(o, n) { return [o.size, ...pick(n).map((i) => o.get("k" + i)), o.keyOf("v1")]; },
        mutate(o) { o.set("extra", "x"); },
    },
    BitSet: {
        build(n) { const x = new S.BitSet(2 * n); for (let i = 0; i < n; i += 3) x.set(perm(i, 2 * n)); return x; },
        probe(o, n) { return [...pick(n).map((i) => o.get(i)), o.nextSet(0), o.nextSet(n)]; },
        mutate(o) { o.flip(1); },
    },
    BloomFilter: {
        build(n) { const x = new S.BloomFilter(10 * n, 7); for (let i = 0; i < n; i++) x.add("k" + i); return x; },
        probe(o, n) { return [o.bits, o.hashes, ...pick(n).map((i) => o.mayContain("k" + i)), o.mayContain("absent-1")]; },
        mutate(o) { o.add("extra"); },
    },
    CountMinSketch: {
        build(n) { const x = new S.CountMinSketch(Math.max(64, n >> 2), 4); for (let i = 0; i < n; i++) x.add("k" + (i % 997), 1 + (i % 3)); return x; },
        probe(o) { return [0, 1, 500, 996, 5000].map((i) => o.count("k" + i)); },
        mutate(o) { o.add("k1", 1000); },
    },
    BTree: {
        build(n) { const x = new S.BTree(); for (let i = 0; i < n; i++) x.set(perm(i, n) * 1.5, "v" + i); return x; },
        probe(o, n) { return [o.size, o.firstKey(), o.lastKey(), ...pick(n).map((i) => o.get(i * 1.5)), o.floorKey(n * 0.7)]; },
        mutate(o) { o.set(-1, "neg"); },
    },
    Deque: {
        build(n) { const x = new S.Deque(); for (let i = 0; i < n; i++) (i & 1 ? x.pushBack(i) : x.pushFront("s" + i)); return x; },
        probe(o) { return seqProbe(o); },
        mutate(o) { o.pushFront("extra"); },
    },
    Fenwick: {
        build(n) { const x = new S.Fenwick(n); for (let i = 0; i < n; i++) x.update(perm(i, n), i % 7); return x; },
        probe(o, n) { return [o.prefixSum(0), o.prefixSum(n >> 1), o.prefixSum(n - 1), o.rangeQuery(1, n >> 2)]; },
        mutate(o) { o.update(0, 99); },
    },
    Graph: {
        build(n) {
            const x = new GraphMod.Graph({ weighted: true });
            for (let i = 0; i < n; i++) x.addNode();
            for (let i = 0; i < n; i++) x.addEdge(i, perm(i, n), 1 + (i % 5));
            return x;
        },
        probe(o, n) { const d = o.bfs(0); return [o.hasEdge(0, perm(0, n)), d.length, d[d.length - 1], o.neighbors(n >> 1)]; },
        mutate(o) { o.addEdge(0, 1, 9); },
    },
    Heap: {
        arg: (a, b) => a - b,
        build(n) { const x = new S.Heap((a, b) => a - b); for (let i = 0; i < n; i++) x.push(perm(i, n)); return x; },
        probe(o) { return [o.size, o.peek()]; },
        mutate(o) { o.push(-5); },
    },
    HyperLogLog: {
        build(n) { const x = new S.HyperLogLog(14); for (let i = 0; i < n; i++) x.add("k" + i); return x; },
        probe(o) { return [o.count()]; },
        mutate(o) { for (let i = 0; i < 2000; i++) o.add("extra" + i); },
    },
    IntervalTree: {
        build(n) { const x = new S.IntervalTree(); for (let i = 0; i < n; i++) { const lo = perm(i, n); x.insert(lo, lo + (i % 10), "v" + i); } return x; },
        probe(o, n) { return [o.size, o.at(n >> 1).length, o.overlapping(3, 9).length]; },
        mutate(o) { o.insert(-10, -5, "neg"); },
    },
    LRU: {
        build(n) { const x = new S.LRU(n); for (let i = 0; i < n + 7; i++) x.set("k" + i, i); return x; },
        probe(o, n) { return [o.size, o.has("k0"), ...pick(n).map((i) => o.get("k" + i))]; },
        mutate(o) { o.set("extra", 1); },
    },
    List: {
        build(n) { const x = new S.List(); for (let i = 0; i < n; i++) (i % 3 ? x.pushBack(i) : x.pushFront({ i })); return x; },
        probe(o) { return seqProbe(o); },
        mutate(o) { o.pushBack("extra"); },
    },
    MinMaxHeap: {
        build(n) { const x = new S.MinMaxHeap(); for (let i = 0; i < n; i++) x.push(perm(i, n), "v" + i); return x; },
        probe(o) { return [o.size, o.peekMin(), o.peekMax()]; },
        mutate(o) { o.push(-1, "neg"); },
    },
    Multimap: {
        build(n) { const x = new S.Multimap(); for (let i = 0; i < n; i++) x.put("k" + (perm(i, n) % Math.max(1, n >> 2)), i); return x; },
        probe(o) { return [o.size, o.get("k0"), o.count("k1")]; },
        mutate(o) { o.put("k0", "extra"); },
    },
    Multiset: {
        build(n) { const x = new S.Multiset(); for (let i = 0; i < n; i++) x.add("k" + perm(i, n), 1 + (i % 3)); return x; },
        probe(o, n) { return [o.size, ...pick(n).map((i) => o.count("k" + i))]; },
        mutate(o) { o.add("k0", 5); },
    },
    RangeMap: {
        build(n) { const x = new S.RangeMap(); for (let i = 0; i < n; i++) { const p = perm(i, n); x.put(p * 10, p * 10 + 5, "v" + i); } return x; },
        probe(o, n) { return [o.get(2), o.get(7), o.get(10 * (n >> 1) + 1), o.entries().length]; },
        mutate(o) { o.put(-20, -10, "neg"); },
    },
    RangeSet: {
        build(n) { const x = new S.RangeSet(); for (let i = 0; i < n; i++) { const p = perm(i, n); x.add(p * 10, p * 10 + 5); } return x; },
        probe(o, n) { return [o.contains(2), o.contains(7), o.contains(10 * (n >> 1) + 1), o.ranges().length]; },
        mutate(o) { o.add(-20, -10); },
    },
    RingBuffer: {
        build(n) { const x = new S.RingBuffer(n >> 1); for (let i = 0; i < n; i++) x.push(i); return x; },
        probe(o) { return seqProbe(o); },
        mutate(o) { o.push("extra"); },
    },
    SegTree: {
        build(n) { const x = new S.SegTree(n, "min"); for (let i = 0; i < n; i++) x.update(perm(i, n), (i * 31) % 1000); return x; },
        probe(o, n) { return [o.rangeQuery(0, n - 1), o.rangeQuery(n >> 2, n >> 1), o.rangeQuery(3, 3)]; },
        mutate(o) { o.update(3, -1); },
    },
    SortedMap: {
        build(n) { const x = new S.SortedMap(); for (let i = 0; i < n; i++) x.set(perm(i, n), "v" + i); return x; },
        probe(o, n) { return [o.size, o.firstKey(), o.lastKey(), ...pick(n).map((i) => o.get(i)), o.rangeQuery(10, 20).length]; },
        mutate(o) { o.set(-1, "neg"); },
    },
    SortedSet: {
        build(n) { const x = new S.SortedSet(); for (let i = 0; i < n; i++) x.add(perm(i, n) * 2); return x; },
        probe(o, n) { return [o.size, o.first(), o.last(), o.floor(n + 1), o.ceil(3)]; },
        mutate(o) { o.add(-1); },
    },
    Table: {
        build(n) { const x = new S.Table(); for (let i = 0; i < n; i++) x.put("r" + (i % 101), "c" + perm(i, n), i); return x; },
        probe(o) { return [o.size, o.get("r0", "c13"), o.row("r1").length]; },
        mutate(o) { o.put("rx", "cx", 1); },
    },
    Trie: {
        build(n) { const x = new S.Trie(); for (let i = 0; i < n; i++) x.insert("w" + perm(i, n).toString(36)); return x; },
        probe(o) { return [o.size, o.has("w1"), o.keysWithPrefix("w1").length, o.longestPrefix("w1zzzz")]; },
        mutate(o) { o.insert("extra"); },
    },
    ZSet: {
        build(n) { const x = new S.ZSet(); for (let i = 0; i < n; i++) x.add("m" + perm(i, n), i % 1000); return x; },
        probe(o, n) { return [o.size, o.score("m1"), o.rank("m" + (n >> 1)), o.range(0, 4)]; },
        mutate(o) { o.add("extra", -1); },
    },
    OrderedMap: {
        build(n) { const x = new S.OrderedMap(); for (let i = 0; i < n; i++) x.set("k" + perm(i, n), [i]); return x; },
        probe(o) { return [o.size, o.firstKey(), o.lastKey(), o.get("k1"), o.at(3)]; },
        mutate(o) { o.set("extra", 1); },
    },
    RoaringBitmap: {
        build(n) { const x = new S.RoaringBitmap(); for (let i = 0; i < n; i++) x.add(perm(i, n) * 3); x.addRange(5000000, 5000000 + n); return x; },
        probe(o, n) { return [o.size, o.has(3), o.has(4), o.rank(n), o.select(n >> 1)]; },
        mutate(o) { o.add(1); },
    },
    TDigest: {
        build(n) { const x = new S.TDigest(); for (let i = 0; i < n; i++) x.add(perm(i, n)); return x; },
        probe(o) { return [o.quantile(0.5), o.quantile(0.99), o.cdf(10)]; },
        mutate(o) { o.add(-1000, 50); },
    },
    TopK: {
        build(n) { const x = new S.TopK(50); for (let i = 0; i < n; i++) x.add("k" + (perm(i, n) % 500), 1 + (i % 5)); return x; },
        probe(o) { return [o.list().slice(0, 5), o.count("k1")]; },
        mutate(o) { o.add("extra", 1e6); },
    },
    CuckooFilter: {
        build(n) { const x = new S.CuckooFilter(2 * n); for (let i = 0; i < n; i++) x.add("k" + i); return x; },
        probe(o, n) { return [o.size, ...pick(n).map((i) => o.mayContain("k" + i))]; },
        mutate(o) { o.add("extra"); },
    },
    IndexedHeap: {
        build(n) { const x = new S.IndexedHeap(); for (let i = 0; i < n; i++) x.push(i, perm(i, n)); return x; },
        probe(o, n) { return [o.size, o.peek(), o.peekPriority(), o.priority(n >> 1)]; },
        mutate(o) { o.push(1e9, -1); },
    },
    KDTree: {
        build(n) { const p = []; for (let i = 0; i < n; i++) p.push([perm(i, n), (i * 13) % 1000]); return new S.KDTree(p); },
        probe(o) { return [o.size, o.nearest([5, 5]), o.radius([100, 100], 20).length]; },
        mutate() {},
    },
    BKDTree: {
        build(n) { const p = []; for (let i = 0; i < n; i++) p.push([perm(i, n), (i * 13) % 1000]); return new S.BKDTree(p, { leafSize: 16 }); },
        probe(o) { return [o.size, o.rangeCount([0, 0], [o.size >> 1, 500]), Array.from(o.range([5, 5], [200, 900]).toArray()).slice(0, 8)]; },
        mutate() {},
    },
    ConsistentHash: {
        build(n) {
            const nodes = Math.max(2, n >> 10), w = Math.min(1000, Math.max(1, Math.floor(n / (4 * nodes))));
            const x = new S.ConsistentHash({ replicas: 4 });
            for (let i = 0; i < nodes; i++) x.addNode("n" + perm(i, nodes), w);
            return x;
        },
        probe(o) { return [o.get("a"), o.get("zz"), o.getN("q", 3)]; },
        mutate(o) { o.addNode("extra"); },
    },
    InvertedIndex: {
        build(n) {
            const x = new S.InvertedIndex({ stopWords: ["the"] });
            for (let i = 0; i < n; i++) x.add(i, words[i % 8] + " " + words[perm(i, 8)] + " w" + (i % 211));
            return x;
        },
        probe(o) { return [o.size, o.docFreq("red"), o.search("red fox").slice(0, 5), o.suggest("w1", 3)]; },
        mutate(o) { o.add(1e7, "extra words"); },
    },
    Interner: {
        build(n) { const x = new S.Interner(); for (let i = 0; i < n; i++) x.intern("s" + perm(i, n)); return x; },
        probe(o, n) { return [o.size, o.lookup("s1"), o.get(n >> 1)]; },
        mutate(o) { o.intern("extra"); },
    },
    UnionFind: {
        build(n) { const x = new S.UnionFind(n); for (let i = 0; i < n; i += 2) x.union(i, perm(i, n)); return x; },
        probe(o, n) { return [o.connected(0, perm(0, n)), o.connected(1, 3), o.find(n - 1)]; },
        mutate(o) { o.union(1, 3); },
    },
    RBTree: {
        build(n) { const x = new S.RBTree(); for (let i = 0; i < n; i++) x.set(perm(i, n), { i }); return x; },
        probe(o, n) { return [o.size, o.firstKey(), o.lastKey(), o.get(n >> 1), o.floorKey(n + 9)]; },
        mutate(o) { o.set(-1, "neg"); },
    },
    WBTree: {
        build(n) { const x = new S.WBTree({ keys: "string" }); for (let i = 0; i < n; i++) x.set("k" + perm(i, n), i); return x; },
        probe(o) { return [o.size, o.firstKey(), o.at(7), o.rank("k5")]; },
        mutate(o) { o.set("extra", 1); },
    },
    SkipList: {
        build(n) { const x = new S.SkipList(); for (let i = 0; i < n; i++) x.set(perm(i, n) - 0.5, [i]); return x; },
        probe(o) { return [o.size, o.firstKey(), o.lastKey(), o.at(5), o.rank(10.5)]; },
        mutate(o) { o.set(-100, "neg"); },
    },
    XArray: {
        build(n) { const x = new S.XArray(); for (let i = 0; i < n; i++) { x.set(perm(i, n) * 3, "v" + i); if (i % 5 === 0) x.mark(perm(i, n) * 3, 1); } x.set(2n ** 63n, "big"); return x; },
        probe(o) { return [o.size, o.get(3), o.isMarked(0, 1), o.nextMarked(1, 1), o.get(2n ** 63n)]; },
        mutate(o) { o.set(1, "extra"); },
    },
    PatriciaTrie: {
        build(n) { const x = new S.PatriciaTrie(); for (let i = 0; i < n; i++) x.set("p" + perm(i, n), i); return x; },
        probe(o) { return [o.size, o.get("p12"), o.longestPrefix("p123zz"), o.withPrefix("p99").length]; },
        mutate(o) { o.set("extra", 1); },
    },
    CSRGraph: {
        build(n) { const e = []; for (let i = 0; i < n; i++) e.push([i, perm(i, n), 1 + (i % 4)]); return new S.CSRGraph(n, e); },
        probe(o, n) { return [o.degree(0), Array.from(o.neighbors(n >> 1)), Array.from(o.bfs(0)).slice(0, 8)]; },
        mutate() {},
    },
    RStarTree: {
        build(n) { const x = new S.RStarTree(2); for (let i = 0; i < n; i++) { const p = perm(i, n); x.insert([p, i % 100], [p + 2, (i % 100) + 3], i); } return x; },
        probe(o) { return [o.size, o.search([0, 0], [10, 10]).sort((a, b) => a - b), o.nearest([50, 50], 3)]; },
        mutate(o) { o.insert([-9, -9], [-8, -8], 1e7); },
    },
    SuffixArray: {
        build(n) { let s = ""; for (let i = 0; i < n; i++) s += "acgt"[perm(i, 4 + (i % 3)) & 3]; return new S.SuffixArray(s); },
        probe(o) { return [o.length, o.count("acg"), o.longestRepeat()]; },
        mutate() {},
    },
};

export function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
