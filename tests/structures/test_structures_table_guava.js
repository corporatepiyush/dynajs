// covers: dyna:structures.Table
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_table_guava");
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

__kitS.section("absent cell", () => {
    const t = new Table();
    t.put("r1", "c1", 11).put("r1", "c2", 12).put("r2", "c1", 21);
    assert(t.get("r1", "c2") === 12 && t.size === 3, "put/get");
    assert(t.get("r9", "c9") === undefined, "absent cell");
    assert(t.has("r1", "c1") && !t.has("r1", "c9"), "has");
    eq(t.row("r1").sort(), [["c1", 11], ["c2", 12]], "row view");
    eq(t.column("c1").sort(), [["r1", 11], ["r2", 21]], "column view");
    assert(t.cells().length === 3, "cells");

    t.put("r1", "c1", 111);
    assert(t.get("r1", "c1") === 111 && t.size === 3, "replace does not grow");
    assert(t.delete("r1", "c1") === true && t.delete("r1", "c1") === false,
           "delete");
    assert(t.size === 2, "size after delete");

    const c = new Table();
    c.put("ab", "c", 1);
    c.put("a", "bc", 2);
    assert(c.size === 2 && c.get("ab", "c") === 1 && c.get("a", "bc") === 2,
           "(row,col) key is unambiguous");
});

__kitS.section("container dropped a reference to a live value", () => {
    function heldWhileReachable(store) {
        let ref;
        const box = { held: null };
        (() => {
            const o = { tag: "payload" };
            ref = new WeakRef(o);
            box.held = store(o);
        })();
        for (let i = 0; i < 3; i++) { const junk = []; for (let j = 0; j < 5000; j++) junk.push({ j }); }
        assert(ref.deref() !== undefined,
               "container dropped a reference to a live value");
        box.held = null;
        return ref;
    }
    function reclaimedAfterDrop(ref) {
        for (let i = 0; i < 5; i++) { const junk = []; for (let j = 0; j < 20000; j++) junk.push({ j }); }
        return ref.deref() === undefined;
    }

    const cases = {
        Multimap: o => { const c = new Multimap(); c.put("k", o); return c; },
        Table:    o => { const c = new Table(); c.put("r", "c", o); return c; },
        RangeMap: o => { const c = new RangeMap(); c.put(0, 10, o); return c; },
        IntervalTree: o => { const c = new IntervalTree(); c.insert(0, 10, o); return c; },
        MinMaxHeap: o => { const c = new MinMaxHeap(); c.push(1, o); return c; },
    };
    for (const name of Object.keys(cases)) {
        const ref = heldWhileReachable(cases[name]);
        assert(reclaimedAfterDrop(ref), name + " leaked its stored value");
    }

    {
        let ref;
        let m = new RangeMap();
        (() => {
            const o = { tag: "split" };
            ref = new WeakRef(o);
            m.put(0, 100, o);
            m.put(40, 60, "hole");
        })();
        assert(m.entries().length === 3, "split produced three spans");
        assert(m.get(10) === m.get(80), "both halves reference the same value");
        m = null;
        for (let i = 0; i < 5; i++) { const junk = []; for (let j = 0; j < 20000; j++) junk.push({ j }); }
        assert(ref.deref() === undefined, "split cell leaked a reference");
    }
    {
        let ref;
        const t = new Table();
        (() => {
            const o = { tag: "replaced" };
            ref = new WeakRef(o);
            t.put("r", "c", o);
        })();
        t.put("r", "c", "new");
        for (let i = 0; i < 5; i++) { const junk = []; for (let j = 0; j < 20000; j++) junk.push({ j }); }
        assert(ref.deref() === undefined, "replaced Table value leaked");
    }
});
__kitS.end();

print("test_structures_guava: all " + n + " assertions passed");
