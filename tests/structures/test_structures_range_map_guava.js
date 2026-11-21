// covers: dyna:structures.RangeMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_range_map_guava");
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

__kitS.section("an enclosed put splits the enclosing span in two", () => {
    const m = new RangeMap();
    m.put(0, 10, "a");
    m.put(3, 5, "b");
    eq(m.entries(), [[0, 3, "a"], [3, 5, "b"], [5, 10, "a"]],
       "an enclosed put splits the enclosing span in two");
    assert(m.get(2) === "a" && m.get(4) === "b" && m.get(7) === "a", "get");
    assert(m.get(-1) === undefined && m.get(10) === undefined, "outside");

    m.put(-5, 4, "c");
    eq(m.entries(), [[-5, 4, "c"], [4, 5, "b"], [5, 10, "a"]],
       "an overlapping put trims the left survivor");

    m.remove(4.5, 6);
    eq(m.entries(), [[-5, 4, "c"], [4, 4.5, "b"], [6, 10, "a"]],
       "remove trims both survivors");

    const d = new RangeMap();
    d.put(0, 1, "same");
    d.put(1, 2, "same");
    assert(d.size === 2, "equal adjacent values stay separate spans");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
