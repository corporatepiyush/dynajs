// covers: dyna:structures.OrderedMap
// Differential tests for the ordered containers added for interpreter-bound
// workloads: ZSet, OrderedMap, IndexedHeap, Interner. Each runs long random
// operation sequences against a brute-force JS model (sorted arrays, Maps) with
// several seeds, and checks every query the class answers after every batch.
// Keys include non-ASCII and astral characters: the order is UTF-8 byte order,
// which the model computes from TextEncoder bytes, not from JS `<`.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_ordered_map_db_ordered");
import { ZSet, OrderedMap, IndexedHeap, Interner } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
function same(a, b, m) { const x = String(JSON.stringify(a)), y = String(JSON.stringify(b)); check(x === y, m + "\n   got  " + x.slice(0, 300) + "\n   want " + y.slice(0, 300)); }

function rng(seed) {
    let s = BigInt(seed) & 0xFFFFFFFFFFFFFFFFn;
    return () => {
        s = (s + 0x9E3779B97F4A7C15n) & 0xFFFFFFFFFFFFFFFFn;
        let z = s;
        z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & 0xFFFFFFFFFFFFFFFFn;
        z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & 0xFFFFFFFFFFFFFFFFn;
        return Number((z ^ (z >> 31n)) >> 11n) / 2 ** 53;
    };
}
const enc = new TextEncoder();
function bcmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b);
    const m = Math.min(x.length, y.length);
    for (let i = 0; i < m; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}
const ALPH = ["a", "b", "c", "ab", "é", "z", "\u{1F600}", "", "A", "0", ""];
function key(r, spread) { let s = ""; const len = 1 + Math.floor(r() * 3); for (let i = 0; i < len; i++) s += ALPH[Math.floor(r() * ALPH.length)]; return s + (spread ? Math.floor(r() * spread) : ""); }

// ---- ZSet -------------------------------------------------------------------
function zsetModel() {
    const m = new Map();
    const sorted = () => [...m].sort((p, q) => p[1] - q[1] || bcmp(p[0], q[0]));
    return { m, sorted };
}
function normRange(start, stop, len) {
    if (start < 0) start += len; if (stop < 0) stop += len; if (start < 0) start = 0;
    if (start > stop || start >= len) return null;
    if (stop >= len) stop = len - 1;
    return [start, stop];
}

for (const seed of [5, 6, 7]) {
    const r = rng(seed), o = new OrderedMap(), M = new Map();
    const sortedKeys = () => [...M.keys()].sort(bcmp);
    for (let step = 0; step < 4000; step++) {
        const op = r(), k = key(r, 30);
        if (op < 0.5) { const v = { v: step }; o.set(k, v); M.set(k, v); }
        else if (op < 0.7) eq(o.delete(k), M.delete(k), `omap[${seed}] delete`);
        else if (op < 0.73) {
            let lo = key(r, 30), hi = key(r, 30); if (bcmp(lo, hi) > 0) [lo, hi] = [hi, lo];
            const want = sortedKeys().filter(x => bcmp(x, lo) >= 0 && bcmp(x, hi) <= 0);
            eq(o.deleteRange(lo, hi), want.length, `omap[${seed}] deleteRange`);
            for (const x of want) M.delete(x);
        }
        if (step % 89 === 0 || step === 3999) {
            const ks = sortedKeys();
            eq(o.size, ks.length, `omap[${seed}] size`);
            same(o.keys(), ks, `omap[${seed}] keys @${step}`);
            check(o.entries().every(([kk, v], i) => kk === ks[i] && v === M.get(kk)), `omap[${seed}] entries identity @${step}`);
            eq(o.firstKey(), ks[0], `omap[${seed}] firstKey`);
            eq(o.lastKey(), ks[ks.length - 1], `omap[${seed}] lastKey`);
            for (let t = 0; t < 15; t++) {
                const q = t < 5 && ks.length ? ks[Math.floor(r() * ks.length)] : key(r, 30);
                eq(o.get(q), M.get(q), `omap[${seed}] get`);
                eq(o.has(q), M.has(q), `omap[${seed}] has`);
                eq(o.floorKey(q), ks.filter(x => bcmp(x, q) <= 0).pop(), `omap[${seed}] floorKey(${q})`);
                eq(o.ceilKey(q), ks.find(x => bcmp(x, q) >= 0), `omap[${seed}] ceilKey(${q})`);
                eq(o.lowerKey(q), ks.filter(x => bcmp(x, q) < 0).pop(), `omap[${seed}] lowerKey(${q})`);
                eq(o.higherKey(q), ks.find(x => bcmp(x, q) > 0), `omap[${seed}] higherKey(${q})`);
                eq(o.rank(q), ks.filter(x => bcmp(x, q) < 0).length, `omap[${seed}] rank(${q})`);
                const i = Math.floor(r() * (ks.length + 4)) - 2, wk = i < 0 ? ks[ks.length + i] : ks[i];
                eq(o.at(i)?.[0], wk, `omap[${seed}] at(${i})`);
                let lo = key(r, 30), hi = key(r, 30); if (bcmp(lo, hi) > 0) [lo, hi] = [hi, lo];
                const inr = ks.filter(x => bcmp(x, lo) >= 0 && bcmp(x, hi) <= 0), lim = Math.floor(r() * 5) + 1;
                same(o.rangeQuery(lo, hi).map(e => e[0]), inr, `omap[${seed}] rangeQuery`);
                same(o.rangeQuery(lo, hi, { limit: lim, reverse: true }).map(e => e[0]), [...inr].reverse().slice(0, lim), `omap[${seed}] rangeQuery rev limit`);
                const p = q.slice(0, 1 + Math.floor(r() * 2));
                const want = ks.filter(x => enc.encode(x).length >= enc.encode(p).length && bcmp(x.length ? x : "", p) >= 0 && enc.encode(x).slice(0, enc.encode(p).length).join() === enc.encode(p).join());
                same(o.prefix(p).map(e => e[0]), want, `omap[${seed}] prefix(${p})`);
            }
            const back = OrderedMap.deserialize(o.serialize());
            same(back.entries(), o.entries(), `omap[${seed}] serialize round trip`);
        }
    }
}

__kitS.section("omap: value holding the map itself", () => {
    const o = new OrderedMap();
    const cyc = { name: "x" }; cyc.self = o; o.set("k", cyc);
    eq(o.get("k").self, o, "omap: value holding the map itself");
    o.set("k", 2); eq(o.get("k"), 2, "omap: replace value");
    same(o.prefix(""), [["k", 2]], "omap: empty prefix is everything");
});
__kitS.end();

print(`test_structures_db_ordered: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
