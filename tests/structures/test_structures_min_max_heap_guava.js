// covers: dyna:structures.MinMaxHeap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_min_max_heap_guava");
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

__kitS.section("empty pops", () => {
    const h = new MinMaxHeap();
    assert(h.popMin() === undefined && h.popMax() === undefined, "empty pops");
    assert(h.peekMin() === undefined && h.peekMax() === undefined, "empty peeks");

    const vals = [];
    for (let i = 0; i < 500; i++) vals.push((i * 2654435761) % 10007);
    vals.forEach(v => h.push(v));
    assert(h.size === 500, "size");
    const sorted = vals.slice().sort((a, b) => a - b);
    assert(h.peekMin() === sorted[0] && h.peekMax() === sorted[499], "peeks");

    const fromMin = [], fromMax = [];
    while (h.size) {
        fromMin.push(h.popMin());
        if (h.size) fromMax.push(h.popMax());
    }
    fromMax.reverse();
    eq(fromMin.concat(fromMax), sorted, "alternating pops reconstruct the sort");

    const p = new MinMaxHeap();
    p.push(3, "low").push(1, "lowest").push(9, "high");
    assert(p.popMin() === "lowest" && p.popMax() === "high", "keyed payloads");

    throws(() => p.push(NaN), RangeError, "NaN priority");

    const t = new MinMaxHeap();
    for (let i = 0; i < 100; i++) t.push(5, i);
    assert(t.size === 100, "ties kept");
    let c = 0;
    while (t.size) { t.popMin(); c++; }
    assert(c === 100, "all ties drain");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
