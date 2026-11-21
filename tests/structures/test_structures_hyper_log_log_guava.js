// covers: dyna:structures.HyperLogLog
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_hyper_log_log_guava");
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

__kitS.section("estimate", () => {
    const h = new HyperLogLog();
    assert(h.precision === 14 && h.registers === 16384, "defaults");
    assert(h.count() === 0, "empty");

    for (let i = 0; i < 50000; i++) h.add("uniq-" + i);
    const est = h.count();
    assert(Math.abs(est - 50000) / 50000 < 0.05,
           "estimate " + est.toFixed(0) + " for 50000 distinct");

    for (let i = 0; i < 50000; i++) h.add("uniq-" + i);
    assert(h.count() === est, "adding a duplicate key is exactly idempotent");

    const small = new HyperLogLog();
    for (let i = 0; i < 200; i++) small.add("s" + i);
    assert(Math.abs(small.count() - 200) / 200 < 0.05,
           "small-range estimate " + small.count().toFixed(1));

    const a = new HyperLogLog(12), b = new HyperLogLog(12);
    for (let i = 0; i < 20000; i++) (i < 12000 ? a : b).add("m" + i);
    a.merge(b);
    assert(Math.abs(a.count() - 20000) / 20000 < 0.06, "merge is a union");
    throws(() => a.merge(h), TypeError, "precision mismatch");
    throws(() => new HyperLogLog(3), RangeError, "precision floor");
    throws(() => new HyperLogLog(19), RangeError, "precision ceiling");
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
