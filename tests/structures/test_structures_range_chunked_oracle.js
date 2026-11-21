// covers: c:src/core/ds/ds-common.c
// F28: RangeSet and RangeMap kept their spans in one gap vector, so every insert away from the gap moved the tail:
// 100K random RangeSet.add took 575 ms and RangeMap.put 1119 ms (O(n) per insert). The spans now live in a chunked
// vector (chunks of 128, split when full, merged when two neighbours fit in half a chunk, a cached chunk hint for
// sequential access): 35 ms and 38 ms, with sequential contains/iterate equal and sequential add +13% (3.7 -> 4.2 ms per 100K, load 7-14).
// Oracle: a sorted-array JS model of the documented semantics (RangeSet unions and merges touching spans; RangeMap
// splits overlaps and never coalesces equal values), driven by seeded random add/put/remove sequences long enough to
// split and merge many chunks; after every batch the ranges, iteration order, size, measure, point probes and a
// serialize/deserialize round trip must equal the model. Differential: the previous gap-vector binary passes the
// three model cases of this file too (only the perf case fails there), so old and new agree with the model.
// Perf oracle: random inserts scale O(n log n), so 4x the spans costs under 8x the CPU (5.3x now; the gap vector measured 11.0x, 10K -> 40K spans, best of 5 CPU, load 12-14).
import { suite } from "../lib/kit.js";
import { RangeSet, RangeMap } from "dyna:structures";
import { cpuNow } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";

const t = suite("test_structures_range_chunked_oracle");

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function bound(r, span) {
    const x = Math.floor(r() * span);
    return r() < 0.2 ? x + 0.5 : x;
}
function setAdd(m, lo, hi) {
    if (!(hi > lo)) return;
    const out = [];
    let L = lo, H = hi, k = 0;
    while (k < m.length && m[k][1] < lo) out.push(m[k++]);
    while (k < m.length && m[k][0] <= H) { L = Math.min(L, m[k][0]); H = Math.max(H, m[k][1]); k++; }
    out.push([L, H]);
    while (k < m.length) out.push(m[k++]);
    m.length = 0; m.push(...out);
}
function cut(m, lo, hi, mk) {
    const out = [];
    for (const e of m) {
        if (e[1] <= lo || e[0] >= hi) { out.push(e); continue; }
        if (e[0] < lo) out.push(mk(e, e[0], lo));
        if (e[1] > hi) out.push(mk(e, hi, e[1]));
    }
    m.length = 0; m.push(...out);
}
const setRemove = (m, lo, hi) => { if (hi > lo) cut(m, lo, hi, (e, a, b) => [a, b]); };
function mapPut(m, lo, hi, v) {
    if (!(hi > lo)) return;
    cut(m, lo, hi, (e, a, b) => [a, b, e[2]]);
    let k = 0;
    while (k < m.length && m[k][0] < lo) k++;
    m.splice(k, 0, [lo, hi, v]);
}
const mapRemove = (m, lo, hi) => { if (hi > lo) cut(m, lo, hi, (e, a, b) => [a, b, e[2]]); };
const covers = (m, x) => m.some((e) => e[0] <= x && x < e[1]);
const mapGet = (m, x) => { const e = m.find((q) => q[0] <= x && x < q[1]); return e ? e[2] : undefined; };

function checkSet(s, m, r, label, { eq, deep }) {
    deep(s.ranges(), m, label + ": ranges");
    deep([...s], m, label + ": iteration order");
    eq(s.size, m.length, label + ": size");
    eq(s.measure, m.reduce((a, e) => a + e[1] - e[0], 0), label + ": measure");
    let bad = 0;
    for (let i = 0; i < 300; i++) { const x = bound(r, 40000); if (s.contains(x) !== covers(m, x)) bad++; }
    eq(bad, 0, label + ": point probes");
    deep(RangeSet.deserialize(s.serialize()).ranges(), m, label + ": serialize round trip");
}
function checkMap(s, m, r, label, { eq, deep }) {
    deep(s.entries(), m, label + ": entries");
    deep([...s], m, label + ": iteration order");
    eq(s.size, m.length, label + ": size");
    let bad = 0;
    for (let i = 0; i < 300; i++) { const x = bound(r, 40000); if (s.get(x) !== mapGet(m, x)) bad++; }
    eq(bad, 0, label + ": point probes");
    deep(RangeMap.deserialize(s.serialize()).entries(), m, label + ": serialize round trip");
}

t.test("RangeSet random add/remove matches the sorted-array model", ({ eq, deep }) => {
    for (const seed of [1, 7, 42]) {
        const r = rng(seed), s = new RangeSet(), m = [];
        for (let batch = 0; batch < 6; batch++) {
            const removeShare = batch < 3 ? 0.15 : 0.55;
            for (let i = 0; i < 900; i++) {
                const lo = bound(r, 40000), hi = lo + 1 + Math.floor(r() * (r() < 0.9 ? 6 : 400));
                if (r() < removeShare) { s.remove(lo, hi); setRemove(m, lo, hi); } else { s.add(lo, hi); setAdd(m, lo, hi); }
            }
            checkSet(s, m, r, "seed " + seed + " batch " + batch, { eq, deep });
        }
        s.remove(-1, 40500); m.length = 0;
        checkSet(s, m, r, "seed " + seed + " emptied", { eq, deep });
    }
});

t.test("RangeSet sequential, reverse and bulk-merging adds", ({ eq, deep }) => {
    const r = rng(9), s = new RangeSet(), m = [];
    for (let i = 0; i < 1000; i++) { s.add(i * 10, i * 10 + 5); setAdd(m, i * 10, i * 10 + 5); }
    for (let i = 1999; i >= 1000; i--) { s.add(i * 10, i * 10 + 5); setAdd(m, i * 10, i * 10 + 5); }
    checkSet(s, m, r, "ascending then descending", { eq, deep });
    s.add(4995, 15003); setAdd(m, 4995, 15003);
    checkSet(s, m, r, "one add swallowing ~1000 spans across chunks", { eq, deep });
    for (let i = 0; i < 2000; i += 3) { s.remove(i * 10 + 1, i * 10 + 2); setRemove(m, i * 10 + 1, i * 10 + 2); }
    checkSet(s, m, r, "punched holes", { eq, deep });
    s.clear(); m.length = 0;
    checkSet(s, m, r, "cleared", { eq, deep });
    s.add(1, 2); setAdd(m, 1, 2);
    checkSet(s, m, r, "reused after clear", { eq, deep });
});

t.test("RangeMap random put/remove matches the split-never-coalesce model", ({ eq, deep }) => {
    for (const seed of [3, 11]) {
        const r = rng(seed), s = new RangeMap(), m = [];
        for (let batch = 0; batch < 5; batch++) {
            for (let i = 0; i < 800; i++) {
                const lo = bound(r, 40000), hi = lo + 1 + Math.floor(r() * (r() < 0.9 ? 8 : 300));
                if (r() < 0.25) { s.remove(lo, hi); mapRemove(m, lo, hi); } else { const v = "v" + Math.floor(r() * 5); s.put(lo, hi, v); mapPut(m, lo, hi, v); }
            }
            checkMap(s, m, r, "seed " + seed + " batch " + batch, { eq, deep });
        }
    }
});

t.test("random inserts scale O(n log n), not O(n^2)", ({ ok, skip }) => {
    const run = (n) => {
        let best = Infinity;
        for (let k = 0; k < 5; k++) {
            const r = rng(5), xs = new Float64Array(n);
            for (let i = 0; i < n; i++) xs[i] = Math.floor(r() * n * 40);
            const t0 = cpuNow(), s = new RangeSet(), mp = new RangeMap();
            for (let i = 0; i < n; i++) { s.add(xs[i], xs[i] + 3); mp.put(xs[i], xs[i] + 3, i); }
            best = Math.min(best, cpuNow() - t0);
        }
        return best;
    };
    const small = run(10000), big = run(40000);
    ok(small > 0 && big > 0, "timed both sizes");
    perfCheck(ok, big < small * 8, "4x the spans: " + (big / small).toFixed(1) + "x CPU (gap vector: 11x)", skip);
});

await t.run();
