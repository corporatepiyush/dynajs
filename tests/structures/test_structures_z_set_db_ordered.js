// covers: dyna:structures.ZSet
// Differential tests for the ordered containers added for interpreter-bound
// workloads: ZSet, OrderedMap, IndexedHeap, Interner. Each runs long random
// operation sequences against a brute-force JS model (sorted arrays, Maps) with
// several seeds, and checks every query the class answers after every batch.
// Keys include non-ASCII and astral characters: the order is UTF-8 byte order,
// which the model computes from TextEncoder bytes, not from JS `<`.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_z_set_db_ordered");
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

for (const seed of [1, 2, 3, 77]) {
    const r = rng(seed), z = new ZSet(), M = zsetModel();
    const scores = [-1, 0, 0.5, 1, 2, 2, 3, 1e300, -Infinity, Infinity, -0];
    for (let step = 0; step < 4000; step++) {
        const op = r();
        const k = key(r, 40);
        if (op < 0.4) {
            const sc = r() < 0.5 ? scores[Math.floor(r() * scores.length)] : Math.floor(r() * 50) - 10;
            const isNew = !M.m.has(k);
            eq(z.add(k, sc), isNew, `zset[${seed}] add new flag`);
            M.m.set(k, sc);
        } else if (op < 0.55) {
            const d = Math.floor(r() * 7) - 3;
            const before = M.m.get(k) ?? 0;
            if (Number.isFinite(before)) { const nv = before + d; eq(z.incrBy(k, d), nv, `zset[${seed}] incrBy`); M.m.set(k, nv); }
        } else if (op < 0.7) {
            eq(z.delete(k), M.m.delete(k), `zset[${seed}] delete`);
        } else if (op < 0.74) {
            const lo = Math.floor(r() * 20) - 5, hi = lo + Math.floor(r() * 10);
            const want = M.sorted().filter(([, s]) => s >= lo && s <= hi);
            eq(z.removeRangeByScore(lo, hi), want.length, `zset[${seed}] removeRangeByScore count`);
            for (const [m] of want) M.m.delete(m);
        } else if (op < 0.77) {
            const s = M.sorted(), a = Math.floor(r() * 10) - 5, b = Math.floor(r() * 10) - 3;
            const nr = normRange(a, b, s.length), want = nr ? s.slice(nr[0], nr[1] + 1) : [];
            eq(z.removeRangeByRank(a, b), want.length, `zset[${seed}] removeRangeByRank(${a},${b})`);
            for (const [m] of want) M.m.delete(m);
        } else if (op < 0.79) {
            const s = M.sorted(), want = s[0];
            same(z.popMin(), want, `zset[${seed}] popMin`);
            if (want) M.m.delete(want[0]);
        } else if (op < 0.81) {
            const s = M.sorted(), want = s[s.length - 1];
            same(z.popMax(), want, `zset[${seed}] popMax`);
            if (want) M.m.delete(want[0]);
        }
        if (step % 97 === 0 || step === 3999) {
            const s = M.sorted();
            eq(z.size, s.length, `zset[${seed}] size @${step}`);
            same(z.entries(), s, `zset[${seed}] entries @${step}`);
            same([...z], s, `zset[${seed}] iterator @${step}`);
            for (let t = 0; t < 12; t++) {
                const kk = t < 6 && s.length ? s[Math.floor(r() * s.length)][0] : key(r, 40);
                const i = s.findIndex(([m]) => m === kk);
                eq(z.rank(kk), i < 0 ? undefined : i, `zset[${seed}] rank(${kk})`);
                eq(z.revRank(kk), i < 0 ? undefined : s.length - 1 - i, `zset[${seed}] revRank`);
                eq(z.score(kk), i < 0 ? undefined : s[i][1], `zset[${seed}] score`);
                eq(z.has(kk), i >= 0, `zset[${seed}] has`);
                const a = Math.floor(r() * 12) - 6, b = Math.floor(r() * 12) - 4;
                const nr = normRange(a, b, s.length);
                same(z.range(a, b), nr ? s.slice(nr[0], nr[1] + 1) : [], `zset[${seed}] range(${a},${b})`);
                same(z.range(a, b, { reverse: true }), nr ? [...s].reverse().slice(nr[0], nr[1] + 1) : [], `zset[${seed}] range rev(${a},${b})`);
                const at = Math.floor(r() * (s.length + 4)) - 2;
                same(z.at(at), at < 0 ? s[s.length + at] : s[at], `zset[${seed}] at(${at})`);
                const lo = Math.floor(r() * 30) - 12, hi = lo + Math.floor(r() * 20) - 2;
                const off = Math.floor(r() * 4), lim = Math.floor(r() * 6) + (r() < 0.3 ? 100 : 0);
                const inr = s.filter(([, x]) => x >= lo && x <= hi);
                eq(z.countByScore(lo, hi), inr.length, `zset[${seed}] countByScore(${lo},${hi})`);
                same(z.rangeByScore(lo, hi), inr, `zset[${seed}] rangeByScore(${lo},${hi})`);
                same(z.rangeByScore(lo, hi, { offset: off, limit: lim }), inr.slice(off, off + lim), `zset[${seed}] rangeByScore off/lim`);
                same(z.rangeByScore(lo, hi, { offset: off, limit: lim, reverse: true }), [...inr].reverse().slice(off, off + lim), `zset[${seed}] rangeByScore rev`);
            }
            same(ZSet.deserialize(z.serialize()).entries(), s, `zset[${seed}] serialize round trip @${step}`);
        }
    }
}

__kitS.section("zset: NaN score is a TypeError", () => {
    const z = new ZSet();
    let threw = false; try { z.add("a", NaN); } catch (e) { threw = e instanceof TypeError; }
    check(threw, "zset: NaN score is a TypeError");
    z.add("a", Infinity);
    threw = false; try { z.incrBy("a", -Infinity); } catch (e) { threw = e instanceof RangeError; }
    check(threw, "zset: incrBy to NaN is a RangeError");
    eq(z.score("a"), Infinity, "zset: failed incrBy leaves the score");
    eq(z.popMin()?.[0], "a", "zset: popMin");
    eq(z.popMin(), undefined, "zset: popMin on empty");
    same(z.range(0, -1), [], "zset: empty range");
    eq(z.add(1, 2), true, "zset: a number member is its string");
    eq(z.has("1"), true, "zset: number member coerced");
});
__kitS.end();

print(`test_structures_db_ordered: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
