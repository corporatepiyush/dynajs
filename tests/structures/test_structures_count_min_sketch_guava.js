// covers: dyna:structures.CountMinSketch
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_count_min_sketch_guava");
import {
    Multiset, Multimap, BiMap, Table, RangeSet, RangeMap,
    IntervalTree, MinMaxHeap, CountMinSketch, HyperLogLog,
} from "dyna:structures";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error((m || "mismatch") + ": got " + a + " want " + b);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

__kitS.section("totalCount is exact", () => {
    const s = new CountMinSketch(4096, 5);
    assert(s.width === 4096 && s.depth === 5 && s.totalCount === 0, "shape");
    for (let i = 0; i < 5000; i++) s.add("item" + (i % 100));
    assert(s.totalCount === 5000, "totalCount is exact");
    for (let i = 0; i < 100; i++)
        assert(s.count("item" + i) >= 50, "never under-counts item" + i);
    s.add("weighted", 1000);
    assert(s.count("weighted") >= 1000, "weighted add");

    const a = new CountMinSketch(256, 3), b = new CountMinSketch(256, 3);
    a.add("x", 5); b.add("x", 7);
    a.merge(b);
    assert(a.count("x") >= 12 && a.totalCount === 12, "merge sums");
    throws(() => a.merge(s), TypeError, "dimension mismatch");
    throws(() => a.merge(a), TypeError, "self-merge would double every count");
    throws(() => new CountMinSketch(0), RangeError, "zero width");
    throws(() => new CountMinSketch(16, 0), RangeError, "zero depth");
    throws(() => new CountMinSketch(16, 65), RangeError, "depth cap");
    throws(() => s.add("k", -1), RangeError, "a sketch cannot be decremented");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
