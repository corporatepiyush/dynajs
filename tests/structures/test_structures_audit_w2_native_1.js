// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the structures sections.
import { Fenwick, BitSet, UnionFind, SegTree, BloomFilter, Trie, LRU, SortedSet, SortedMap, Heap, List, Deque, RingBuffer } from "dyna:structures";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- S1-01 -------------------------------------------------------------------
{
    const cases = [
        ["Fenwick", Fenwick, () => { const f = new Fenwick(512); for (let i = 0; i < 512; i++) f.update(i, i + 0.5); return f; }, (c) => new c(1)],
        ["BitSet", BitSet, () => { const b = new BitSet(); for (let i = 0; i < 4096; i += 3) b.set(i); return b; }, (c) => new c()],
        ["UnionFind", UnionFind, () => { const u = new UnionFind(512); for (let i = 1; i < 512; i += 2) u.union(i - 1, i); return u; }, (c) => new c(1)],
        ["SegTree", SegTree, () => { const s = new SegTree(256, "sum"); for (let i = 0; i < 256; i++) s.update(i, i); return s; }, (c) => new c(1, "sum")],
        ["BloomFilter", BloomFilter, () => { const b = new BloomFilter(4096, 0.01); for (let i = 0; i < 100; i++) b.add("k" + i); return b; }, (c) => new c(8, 0.5)],
        ["Trie", Trie, () => { const t = new Trie(); for (let i = 0; i < 100; i++) t.insert("word" + i); return t; }, (c) => new c()],
        ["LRU", LRU, () => { const l = new LRU(64); for (let i = 0; i < 64; i++) l.set("k" + i, i); return l; }, (c) => new c(1)],
        ["SortedSet", SortedSet, () => { const s = new SortedSet(); for (let i = 0; i < 200; i++) s.add(i); return s; }, (c) => new c()],
        ["SortedMap", SortedMap, () => { const s = new SortedMap(); for (let i = 0; i < 200; i++) s.set(i, i); return s; }, (c) => new c()],
        ["Heap", Heap, () => { const h = new Heap(); for (let i = 0; i < 200; i++) h.push(i); return h; }, (c) => new c()],
        ["List", List, () => { const l = new List(); for (let i = 0; i < 200; i++) l.pushBack(i); return l; }, (c) => new c()],
        ["Deque", Deque, () => { const d = new Deque(); for (let i = 0; i < 200; i++) d.pushBack(i); return d; }, (c) => new c()],
        ["RingBuffer", RingBuffer, () => { const r = new RingBuffer(64); for (let i = 0; i < 64; i++) r.push(i); return r; }, (c) => new c(1)],
    ];
    for (const [name, C, build, small] of cases) {
        let bytes;
        try { bytes = build().serialize(); } catch (e) { ok(false, name + ": cannot build the fixture (" + e.message + ")"); continue; }
        const honest = C.deserialize(bytes);
        const saved = C.prototype.constructor;
        let hijacked = 0, viaObj, viaSmall;
        try {
            C.prototype.constructor = function () { hijacked++; return {}; };
            viaObj = C.deserialize(bytes);
            C.prototype.constructor = function () { hijacked++; return small(saved); };
            viaSmall = C.deserialize(bytes);
        } catch (e) {
            ok(false, name + ".deserialize threw under a replaced constructor: " + e.message);
        } finally {
            C.prototype.constructor = saved;
        }
        eq(hijacked, 0, name + ".deserialize never calls prototype.constructor");
        ok(viaObj instanceof C && viaSmall instanceof C, name + ".deserialize returns a genuine instance");
        let same = false;
        try {
            const a = honest.serialize(), b = viaSmall.serialize();
            same = a.length === b.length && a.every((v, i) => v === b[i]);
        } catch (e) { }
        ok(same, name + ".deserialize result is unaffected by the replaced constructor");
    }
}
if (failures) {
    print("test_structures_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_structures_audit_w2_native_1 failed");
}
print("test_structures_audit_w2_native_1: " + checks + " checks passed");
