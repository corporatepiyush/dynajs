// Heap.pop with a comparator walks to a leaf comparing children only (one
// call per level), then climbs back to place the displaced last element, and
// only then moves anything -- so the comparator never sees a half-moved
// array. The oracle is Array.prototype.sort over the same keys; ties may come
// out in any order (a heap is not stable), so only keys are compared.
import { Heap } from "dyna:structures";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }
let seed = 99;
const rnd = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296;
const byKey = (a, b) => a.k - b.k;

// 1. drain order over sizes that cross every tree depth, with many ties
{
    let bad = "";
    for (const size of [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 100, 255, 256, 257, 1000, 4097]) {
        for (const span of [3, 1000]) {
            const h = new Heap(byKey), keys = [];
            for (let i = 0; i < size; i++) { const k = Math.floor(rnd() * span); keys.push(k); h.push({ k, i }); }
            keys.sort((a, b) => a - b);
            const got = [];
            while (h.size) got.push(h.pop().k);
            if (got.join() !== keys.join()) { bad = size + "/" + span; break; }
        }
        if (bad) break;
    }
    ok(bad === "", "pop drains in key order at every size", bad);
}

// 2. pushes and pops interleaved against a sorted-array model
{
    const h = new Heap(byKey), model = [];
    let bad = -1;
    for (let step = 0; step < 20000 && bad < 0; step++) {
        if (model.length === 0 || rnd() < 0.55) {
            const k = Math.floor(rnd() * 50);
            h.push({ k }); model.push(k); model.sort((a, b) => a - b);
        } else {
            const v = h.pop(), want = model.shift();
            if (v.k !== want || h.size !== model.length) bad = step;
        }
        if (bad < 0 && model.length && h.peek().k !== model[0]) bad = step;
    }
    ok(bad < 0, "interleaved push/pop matches the model", bad);
}

// 3. a comparator that reads the heap mid-pop sees a consistent heap: the
//    same size every time, and peek never changes during one pop
{
    let h, sizes = new Set(), peeks = new Set(), during = false;
    h = new Heap((a, b) => { if (during) { sizes.add(h.size); peeks.add(h.peek()); } return a - b; });
    for (let i = 0; i < 500; i++) h.push((i * 7919) % 1000);
    let good = true;
    for (let i = 0; i < 100 && good; i++) {
        sizes.clear(); peeks.clear();
        during = true;
        h.pop();
        during = false;
        if (sizes.size > 1 || peeks.size > 1) good = false;
    }
    ok(good, "the heap reads the same from inside its comparator throughout a pop");
}

// 4. a comparator that throws mid-pop loses and duplicates nothing beyond
//    the root it was popping (already removed when the comparison runs)
{
    let armed = 0;
    const h = new Heap((a, b) => { if (armed && --armed === 0) throw new Error("boom"); return a - b; });
    const model = [];
    for (let i = 0; i < 300; i++) { const v = (i * 37) % 301; model.push(v); h.push(v); }
    let threw = 0;
    for (let round = 0; round < 50; round++) {
        const top = h.peek();
        armed = 1 + Math.floor(rnd() * 12);
        try { h.pop(); } catch (e) { threw++; }
        armed = 0;
        model.splice(model.indexOf(top), 1);
    }
    const rest = [];
    while (h.size) rest.push(h.pop());
    ok(threw > 0, "the throwing comparator did throw", threw);
    // order is not promised after a throw (the displaced element is left at
    // the root and no comparator is available to place it); content is
    ok(rest.slice().sort((a, b) => a - b).join() === model.slice().sort((a, b) => a - b).join(),
        "after comparator throws, every remaining element is there exactly once", rest.length + " vs " + model.length);
}

print("test_structures_heap_order: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
