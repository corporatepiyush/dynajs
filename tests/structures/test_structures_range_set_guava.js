// covers: dyna:structures.RangeSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_range_set_guava");
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

__kitS.section("two disjoint spans", () => {
    const r = new RangeSet();
    r.add(1, 5).add(10, 15);
    eq(r.ranges(), [[1, 5], [10, 15]], "two disjoint spans");
    r.add(5, 10);
    eq(r.ranges(), [[1, 15]], "adjacent spans coalesce");
    assert(r.size === 1 && r.measure === 14, "size and measure");

    assert(r.contains(1) && r.contains(14.999) && !r.contains(15),
           "half-open: lo is in, hi is out");
    assert(!r.contains(0.999), "below");
    assert(r.encloses(2, 6) && !r.encloses(14, 16), "encloses");
    assert(r.intersects(14, 20) && !r.intersects(15, 20), "intersects");

    r.remove(4, 6);
    eq(r.ranges(), [[1, 4], [6, 15]], "remove splits");
    eq(r.complement(0, 20), [[0, 1], [4, 6], [15, 20]], "complement");

    const before = JSON.stringify(r.ranges());
    r.add(5, 5);
    r.add(9, 3);
    r.remove(7, 7);
    eq(r.ranges(), JSON.parse(before), "empty/inverted intervals are no-ops");

    r.clear();
    assert(r.size === 0 && r.measure === 0, "clear");

    throws(() => r.add(NaN, 5), RangeError, "NaN lo");
    throws(() => r.add(0, NaN), RangeError, "NaN hi");

    const inf = new RangeSet();
    inf.add(-Infinity, 0);
    assert(inf.contains(-1e308) && !inf.contains(0), "unbounded below");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
