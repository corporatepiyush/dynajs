// covers: dyna:structures.BiMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_bi_map_guava");
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

__kitS.section("both directions", () => {
    const b = new BiMap();
    b.set("one", "1").set("two", "2");
    assert(b.get("one") === "1" && b.keyOf("2") === "two", "both directions");
    assert(b.get("nope") === undefined && b.keyOf("nope") === undefined, "absent");
    assert(b.has("one") && b.hasValue("1"), "has/hasValue");
    assert(b.size === 2, "size");

    throws(() => b.set("three", "1"), TypeError, "duplicate value must throw");
    assert(b.get("three") === undefined && b.keyOf("1") === "one",
           "the refused set changed nothing");

    b.forceSet("three", "1");
    assert(b.keyOf("1") === "three" && b.get("one") === undefined,
           "forceSet evicts the old pair entirely");
    assert(b.size === 2, "forceSet did not grow the map");

    b.set("two", "22");
    assert(b.get("two") === "22" && b.keyOf("2") === undefined, "rebind key");

    assert(b.delete("two") === true && b.delete("two") === false, "delete");
    assert(b.deleteValue("1") === true, "deleteValue");
    assert(b.size === 0, "empty after deletes");

    b.set("k", "v");
    eq(b.entries(), [["k", "v"]], "entries");
    eq(b.inverseEntries(), [["v", "k"]], "inverseEntries");
    b.clear();
    assert(b.size === 0, "clear");

    const i = new BiMap();
    i.set("a", "1");
    i.set("a", "1");
    assert(i.size === 1, "idempotent set");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
