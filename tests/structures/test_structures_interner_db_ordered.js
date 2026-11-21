// covers: dyna:structures.Interner
// Differential tests for the ordered containers added for interpreter-bound
// workloads: ZSet, OrderedMap, IndexedHeap, Interner. Each runs long random
// operation sequences against a brute-force JS model (sorted arrays, Maps) with
// several seeds, and checks every query the class answers after every batch.
// Keys include non-ASCII and astral characters: the order is UTF-8 byte order,
// which the model computes from TextEncoder bytes, not from JS `<`.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_interner_db_ordered");
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

__kitS.section("interner: intern id", () => {
    const r = rng(21), it = new Interner(), M = new Map(), rev = [];
    for (let step = 0; step < 20000; step++) {
        const s = r() < 0.01 ? "x".repeat(70000 + step) : key(r, 2000);
        if (!M.has(s)) { M.set(s, rev.length); rev.push(s); }
        eq(it.intern(s), M.get(s), "interner: intern id");
    }
    eq(it.size, rev.length, "interner: size");
    check(rev.every((s, i) => it.get(i) === s && it.lookup(s) === i && it.has(s)), "interner: get/lookup for every id");
    eq(it.lookup("never-seen-key"), undefined, "interner: lookup miss");
    eq(it.get(rev.length), undefined, "interner: get past the end");
    eq(it.get(-1), undefined, "interner: negative id");
    eq(it.get(1.5), undefined, "interner: fractional id");
    const batch = rev.slice(0, 500).concat(["new-a", "new-b", "new-a"]);
    const ids = it.internMany(batch);
    check(ids instanceof Uint32Array && ids.length === batch.length, "interner: internMany returns Uint32Array");
    check(batch.every((s, i) => it.get(ids[i]) === s), "interner: internMany ids resolve");
    eq(ids[batch.length - 1], ids[batch.length - 3], "interner: duplicate in a batch shares an id");
    same(it.getMany([0, 1, 1e9]), [rev[0], rev[1], undefined], "interner: getMany with unknown id");
    const back = Interner.deserialize(it.serialize());
    check(back.size === it.size && rev.every((s, i) => back.get(i) === s), "interner: serialize preserves ids");
    let threw = false; try { it.internMany(["a", 1]); } catch (e) { threw = e instanceof TypeError; } check(threw, "interner: non-string in internMany refused");
});
__kitS.end();

print(`test_structures_db_ordered: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
