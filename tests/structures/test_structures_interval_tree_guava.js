// covers: dyna:structures.IntervalTree
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_interval_tree_guava");
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

__kitS.section("point overlap", () => {
    const t = new IntervalTree();
    t.insert(1, 5, "a").insert(4, 9, "b").insert(20, 25, "c");
    assert(t.size === 3, "size");
    eq(t.overlapping(4, 4).map(x => x[2]).sort(), ["a", "b"], "point overlap");
    eq(t.at(21).map(x => x[2]), ["c"], "at()");
    eq(t.at(10), [], "gap");
    eq(t.overlapping(0, 100).length, 3, "everything");
    eq(t.overlapping(9, 20).map(x => x[2]).sort(), ["b", "c"],
       "closed intervals include their endpoints");

    const big = new IntervalTree();
    for (let i = 0; i < 500; i++) big.insert(i, i + 3, i);
    for (let round = 0; round < 3; round++) {
        const hits = big.overlapping(100, 100).map(x => x[2]).sort((a, b) => a - b);
        eq(hits, [97, 98, 99, 100], "round " + round);
        big.insert(1000 + round, 1000 + round, "extra" + round);
    }
    assert(big.size === 503, "size after interleaved insert/query");
    throws(() => big.at(NaN), RangeError, "NaN query");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
