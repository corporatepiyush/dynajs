// covers: contract dyna:structures.SortedMap.deserialize dyna:structures.SortedSet.deserialize dyna:structures.LRU.deserialize dyna:structures.Heap.deserialize -- one rebuild loop: every record is replayed through the instance's own method
// deserialize()/load() rebuild SortedMap, SortedSet, LRU and Heap by calling the instance's set/add/put/push once per
// stored entry, looked up on the instance for every call (so a patched prototype method is honoured, even one that
// swaps itself out mid-rebuild). The method name was converted to an atom on every call and each value fetched by a
// property get; the atom is now made once per rebuild and the values are read from the internal value array
// directly. Oracle: round trips equal a JS model (keys, values, order, object identity inside one record); a patched
// prototype method sees exactly the stored arguments in stored order; a method that replaces itself after k calls
// hands the remaining calls to the replacement; a throwing method aborts the rebuild with its error.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_deserialize_rebuild_invoke_oracle");
import { SortedMap, SortedSet, LRU, Heap } from "dyna:structures";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
let seed = 31;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const sizes = [0, 1, 2, 17, 1000, 20000];
__kitS.section("round trips equal a JS model", () => {
    for (const n of sizes) {
        const m = new SortedMap(), model = new Map();
        for (let i = 0; i < n; i++) { const k = (rnd() * 1e6) | 0, v = i % 3 ? { i, s: "v" + i } : i % 2 ? "s" + i : i * 0.5; m.set(k, v); model.set(k, v); }
        const back = SortedMap.deserialize(m.serialize());
        const want = [...model.entries()].sort((a, b) => a[0] - b[0]);
        const got = [...back.entries()];
        ok(got.length === want.length && got.every(([k, v], i) => k === want[i][0] && JSON.stringify(v) === JSON.stringify(want[i][1])), "SortedMap n=" + n);
        const s = new SortedSet(); for (const k of model.keys()) s.add(k);
        ok(JSON.stringify(SortedSet.deserialize(s.serialize()).toArray()) === JSON.stringify(want.map((e) => e[0])), "SortedSet n=" + n);
        const l = new LRU(n + 1); for (let i = 0; i < n; i++) l.set("k" + i, [i, "x" + i]);
        const lb = LRU.deserialize(l.serialize());
        ok(lb.size === n && (n === 0 || (JSON.stringify(lb.get("k0")) === "[0,\"x0\"]" && JSON.stringify(lb.get("k" + (n - 1))) === JSON.stringify([n - 1, "x" + (n - 1)]))), "LRU n=" + n);
        const h = new Heap(); const hv = []; for (let i = 0; i < n; i++) { const x = (rnd() * 1e6) | 0; h.push(x); hv.push(x); }
        const hb = Heap.deserialize(h.serialize()); hv.sort((a, b) => a - b);
        const popped = []; while (hb.size) popped.push(hb.pop());
        ok(popped.length === n && popped.every((x, i) => x === hv[i]), "Heap n=" + n);
    }
});
__kitS.section("a patched prototype method sees every stored entry in order", () => {
    const m = new SortedMap(); for (let i = 0; i < 500; i++) m.set(i * 7, "v" + i);
    const bytes = m.serialize();
    const orig = SortedMap.prototype.set, seen = [];
    SortedMap.prototype.set = function (k, v) { seen.push(k + "=" + v); return orig.call(this, k, v); };
    let back;
    try { back = SortedMap.deserialize(bytes); } finally { SortedMap.prototype.set = orig; }
    ok(seen.length === 500 && seen.every((s, i) => s === i * 7 + "=v" + i), "patched set saw " + seen.length + " calls in order");
    ok(back.size === 500, "rebuilt size");
    const l = new LRU(100); for (let i = 0; i < 50; i++) l.set("a" + i, i);
    const lo = LRU.prototype.put; let calls = 0;
    LRU.prototype.put = function (k, v) { calls++; return lo.call(this, k, v); };
    try { LRU.deserialize(l.serialize()); } catch (e) { calls = -1; } finally { LRU.prototype.put = lo; }
    ok(calls === 50, "patched LRU put called " + calls + " times");
    const h = new Heap(); for (let i = 0; i < 40; i++) h.push(i);
    const hp = Heap.prototype.push; let hc = 0;
    Heap.prototype.push = function (x) { hc++; return hp.call(this, x); };
    try { Heap.deserialize(h.serialize()); } finally { Heap.prototype.push = hp; }
    ok(hc === 40, "patched Heap push called " + hc + " times");
});
__kitS.section("a method that replaces itself mid-rebuild hands over the rest", () => {
    const m = new SortedMap(); for (let i = 0; i < 100; i++) m.set(i, i);
    const orig = SortedMap.prototype.set; let a = 0, b = 0;
    const second = function (k, v) { b++; return orig.call(this, k, v); };
    SortedMap.prototype.set = function (k, v) { a++; if (a === 30) SortedMap.prototype.set = second; return orig.call(this, k, v); };
    try { SortedMap.deserialize(m.serialize()); } finally { SortedMap.prototype.set = orig; }
    ok(a === 30 && b === 70, "first method " + a + " calls, replacement " + b);
    const s = new SortedSet(); for (let i = 0; i < 10; i++) s.add(i);
    const sa = SortedSet.prototype.add; let thrown = null;
    SortedSet.prototype.add = function (k) { if (k === 6) throw new Error("stop at 6"); return sa.call(this, k); };
    try { SortedSet.deserialize(s.serialize()); } catch (e) { thrown = e.message; } finally { SortedSet.prototype.add = sa; }
    ok(thrown === "stop at 6", "a throwing method aborts the rebuild: " + thrown);
});
print("deserialize rebuild invoke oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
