// covers: dyna:structures.Multimap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_multimap_guava");
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

__kitS.section("get preserves insertion order", () => {
    const m = new Multimap();
    assert(m.size === 0 && m.keyCount === 0, "empty");
    m.put("a", 1).put("a", 2).put("b", 3);
    eq(m.get("a"), [1, 2], "get preserves insertion order");
    eq(m.get("missing"), [], "absent key is an empty array, not undefined");
    assert(m.count("a") === 2 && m.size === 3 && m.keyCount === 2, "counts");

    m.put("a", 1);
    eq(m.get("a"), [1, 2, 1], "list flavour keeps duplicates");

    assert(m.removeAt("a", 1) === 2, "removeAt returns the removed value");
    eq(m.get("a"), [1, 1], "removeAt preserves the order of the rest");
    assert(m.removeAt("a", 99) === undefined, "removeAt out of range");
    assert(m.removeAt("nope", 0) === undefined, "removeAt absent key");

    assert(m.delete("a") === 2, "delete returns how many values went");
    assert(m.count("a") === 0 && m.keyCount === 1, "key gone");
    assert(m.delete("a") === 0, "delete of an absent key");

    m.put("z", "v");
    eq(m.keys().sort(), ["b", "z"], "keys");
    eq(m.entries().sort(), [["b", 3], ["z", "v"]], "entries is one pair per VALUE");

    const k = new Multimap();
    k.put("only", 7);
    k.removeAt("only", 0);
    assert(k.keyCount === 0 && k.size === 0, "emptied key is removed");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
