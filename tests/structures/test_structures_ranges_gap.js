// RangeSet and RangeMap keep their sorted spans in a gap buffer: the free
// slots sit where the last edit happened, so appends, front inserts and
// in-place overwrites are O(1) instead of a memmove of the whole array, and
// lookups search the run before the gap and the run after it separately.
// The oracle here is a plain JS model over an explicit sorted array. Every
// pattern that moves the gap is driven -- ascending, descending, alternating
// ends, random, overwrite in place, growth while the gap is in the middle --
// and after each step both the answers and the full span list must match.
import { RangeSet, RangeMap } from "dyna:structures";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }
let seed = 4242;
const rnd = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296;

// --- model ------------------------------------------------------------------
function setAdd(m, lo, hi) {
    if (!(hi > lo)) return;
    const out = [];
    let a = lo, b = hi, placed = false;
    for (const [x, y] of m) {
        if (y < a) out.push([x, y]);
        else if (x > b) { if (!placed) { out.push([a, b]); placed = true; } out.push([x, y]); }
        else { a = Math.min(a, x); b = Math.max(b, y); }
    }
    if (!placed) out.push([a, b]);
    m.length = 0; m.push(...out);
}
function setRemove(m, lo, hi) {
    if (!(hi > lo)) return;
    const out = [];
    for (const [x, y] of m) {
        if (y <= lo || x >= hi) { out.push([x, y]); continue; }
        if (x < lo) out.push([x, lo]);
        if (y > hi) out.push([hi, y]);
    }
    m.length = 0; m.push(...out);
}
// the model's spans are sorted and disjoint: find the last one starting <= x
function spanAt(m, x) {
    let lo = 0, hi = m.length - 1, at = -1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (m[mid][0] <= x) { at = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return at >= 0 && x < m[at][1] ? m[at] : null;
}
const setContains = (m, x) => spanAt(m, x) !== null;
function mapPut(m, lo, hi, v) {
    if (!(hi > lo)) return;
    mapRemove(m, lo, hi);
    m.push([lo, hi, v]); m.sort((p, q) => p[0] - q[0]);
}
function mapRemove(m, lo, hi) {
    if (!(hi > lo)) return;
    const out = [];
    for (const [x, y, v] of m) {
        if (y <= lo || x >= hi) { out.push([x, y, v]); continue; }
        if (x < lo) out.push([x, lo, v]);
        if (y > hi) out.push([hi, y, v]);
    }
    m.length = 0; m.push(...out);
}
const mapGet = (m, x) => { const e = spanAt(m, x); return e ? e[2] : undefined; };

// --- drivers ----------------------------------------------------------------
const PATTERNS = {
    ascending: (i) => i * 10,
    descending: (i, N) => (N - i) * 10,
    alternating: (i, N) => (i % 2 ? i : N * 2 - i) * 10,
    random: () => Math.floor(rnd() * 5000) * 3,
    clustered: (i) => (i % 50) * 100 + Math.floor(i / 50),
};

for (const [name, at] of Object.entries(PATTERNS)) {
    const N = 600;
    const s = new RangeSet(), m = [];
    const mp = new RangeMap(), mm = [];
    let bad = "";
    for (let i = 0; i < N && !bad; i++) {
        const lo = at(i, N), w = 1 + Math.floor(rnd() * 12);
        const op = rnd();
        if (op < 0.65) { s.add(lo, lo + w); setAdd(m, lo, lo + w); }
        else if (op < 0.85) { s.remove(lo, lo + w); setRemove(m, lo, lo + w); }
        else { s.add(lo, lo + w); setAdd(m, lo, lo + w); s.add(lo, lo + w); setAdd(m, lo, lo + w); }
        const v = "v" + i;
        if (op < 0.7) { mp.put(lo, lo + w, v); mapPut(mm, lo, lo + w, v); }
        else if (op < 0.85) { mp.remove(lo + 1, lo + w + 3); mapRemove(mm, lo + 1, lo + w + 3); }
        else { mp.put(lo, lo + w, v + "x"); mapPut(mm, lo, lo + w, v + "x"); mp.put(lo, lo + w, v); mapPut(mm, lo, lo + w, v); }
        // exact endpoints: lo is inside, hi is not (half-open); the span next
        // to the gap is the one a wrong boundary test would misread
        for (const [a, b] of m) {
            if (!s.contains(a) || s.contains(b) !== setContains(m, b)) { bad = "contains at an endpoint [" + a + "," + b + ") after step " + i; break; }
        }
        for (const [a, b] of mm) {
            if (mp.get(a) !== mapGet(mm, a) || mp.get(b) !== mapGet(mm, b)) { bad = "get at an endpoint [" + a + "," + b + ") after step " + i; break; }
        }
        if (bad) break;
        if (JSON.stringify(s.ranges()) !== JSON.stringify(m)) bad = "set spans after step " + i;
        else if (JSON.stringify(mp.entries()) !== JSON.stringify(mm)) bad = "map entries after step " + i;
        else {
            for (let k = 0; k < 6 && !bad; k++) {
                const x = Math.floor(rnd() * 6200) - 50 + rnd();
                if (s.contains(x) !== setContains(m, x)) bad = "contains(" + x + ") after step " + i;
                else if (mp.get(x) !== mapGet(mm, x)) bad = "get(" + x + ") after step " + i;
                else if (s.intersects(x, x + 4) !== m.some(([a, b]) => a < x + 4 && x < b)) bad = "intersects after step " + i;
                else if (s.encloses(x, x + 2) !== m.some(([a, b]) => a <= x && x + 2 <= b)) bad = "encloses after step " + i;
            }
        }
    }
    ok(!bad, name + ": RangeSet and RangeMap match the model at every step", bad);
    ok(s.size === m.length && mp.size === mm.length, name + ": sizes match", s.size + "/" + m.length);
    // the record carries the logical order, whatever the gap was doing
    const s2 = RangeSet.deserialize(s.serialize()), m2 = RangeMap.deserialize(mp.serialize());
    ok(JSON.stringify(s2.ranges()) === JSON.stringify(m) && JSON.stringify(m2.entries()) === JSON.stringify(mm), name + ": serialize round trip");
    const cmp = s.complement(-100, 7000), want = [];
    let cur = -100;
    for (const [a, b] of m) { if (a > cur) want.push([cur, Math.min(a, 7000)]); cur = Math.max(cur, b); }
    if (cur < 7000) want.push([cur, 7000]);
    ok(JSON.stringify(cmp) === JSON.stringify(want.filter(([a, b]) => b > a)), name + ": complement");
}

// growth while the gap is in the middle: fill to a power of two from the
// front, then insert into the middle so the next insert must reallocate
{
    const s = new RangeSet(), m = [];
    for (let i = 0; i < 64; i++) { s.add(i * 4, i * 4 + 1); setAdd(m, i * 4, i * 4 + 1); }
    for (let i = 0; i < 300; i++) { const lo = 128 + (i % 7) + i * 0.001; s.add(lo, lo + 0.0005); setAdd(m, lo, lo + 0.0005); }
    s.clear();
    ok(s.size === 0 && s.ranges().length === 0 && !s.contains(4), "clear empties a set whose gap was in the middle");
    s.add(1, 2);
    ok(JSON.stringify(s.ranges()) === "[[1,2]]", "and it works afterwards");
}

print("test_structures_ranges_gap: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
