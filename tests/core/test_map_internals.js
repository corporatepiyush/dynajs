// flags: --std
// Map/Set internals: a dense insertion-ordered entry array with a control-byte index above 8
// entries, tombstones on delete, compaction that re-indexes live iterators and forEach, and a
// string hash cached in non-atom strings. Expected values follow ECMA-262 24.1/24.2 (iteration
// visits entries added during it, skips deleted ones, and clear() keeps iterators usable);
// node 26 agrees on every case that does not use std.
import * as std from "std";
const print = globalThis.print ?? console.log;
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const gc = () => { if (typeof std !== "undefined" && std.gc) std.gc(); };

for (const n of [3, 8, 9, 40, 2000]) {
    const m = new Map();
    for (let i = 0; i < n; i++) m.set(i, i * 10);
    const seen = [];
    for (const [k] of m) {
        seen.push(k);
        if (k === 0) m.delete(1);
        if (k === 2) m.set(n + 1, 0);
    }
    const want = [];
    for (let i = 0; i < n; i++) if (i !== 1) want.push(i);
    want.push(n + 1);
    eq(seen, want, `n=${n}: iteration skips a deleted entry ahead and visits one added`);
    eq(m.size, n, `n=${n}: size after delete one, add one`);
}
for (const n of [10, 500, 5000]) {
    const m = new Map();
    for (let i = 0; i < n; i++) m.set("k" + i, i);
    const it = m.keys();
    eq(it.next().value, "k0", `n=${n}: first key`);
    for (let i = 0; i < n - 2; i++) m.delete("k" + i);
    for (let i = 0; i < n; i++) m.set("x" + i, i);
    const rest = [...it];
    eq(rest.length, 2 + n, `n=${n}: an iterator survives mass delete + regrowth (compaction)`);
    eq(rest[0], "k" + (n - 2), `n=${n}: it resumes at the first live entry`);
    eq(rest[rest.length - 1], "x" + (n - 1), `n=${n}: and reaches the last added entry`);
}
{
    const m = new Map([[1, 1], [2, 2], [3, 3]]);
    const it = m.values();
    it.next();
    m.clear();
    m.set(9, 9);
    eq([...it], [9], "an iterator after clear() sees entries added after the clear");
    eq(m.size, 1, "size after clear and one set");
    const it2 = m.entries();
    m.clear();
    eq(it2.next(), { value: undefined, done: true }, "an iterator over a cleared map is done");
    m.set(1, 1);
    eq(it2.next(), { value: undefined, done: true }, "and stays done");
}
{
    const m = new Map();
    for (let i = 0; i < 100; i++) m.set(i, i);
    const order = [];
    m.forEach((v, k, map) => {
        order.push(k);
        if (k === 5) for (let j = 6; j < 95; j++) map.delete(j);
        if (k === 95) for (let j = 0; j < 200; j++) map.set(1000 + j, j);
    });
    eq(order.length, 6 + 5 + 200, "forEach across a compaction and a regrowth");
    eq(order.slice(0, 8), [0, 1, 2, 3, 4, 5, 95, 96], "forEach skips the deleted range");
    eq(order[order.length - 1], 1199, "forEach visits the appended entries");
    let thrown = null;
    try { m.forEach(() => { throw new Error("stop"); }); } catch (e) { thrown = e.message; }
    eq(thrown, "stop", "forEach propagates a throw");
    m.set("after", 1);
    eq(m.get("after"), 1, "the map is usable after a throwing forEach");
}
{
    const s = new Set();
    for (let i = 0; i < 20; i++) s.add(i);
    for (let i = 0; i < 20; i++) if (i % 3) s.delete(i);
    eq([...s], [0, 3, 6, 9, 12, 15, 18], "Set keeps insertion order across deletes");
    s.add(1);
    s.add(0);
    eq([...s].slice(-2), [18, 1], "re-adding a present key keeps its position, a deleted one goes last");
    const m = new Map();
    for (let i = 0; i < 70; i++) m.set(i, i);
    for (let i = 0; i < 69; i++) m.delete(i);
    eq([...m.keys()], [69], "shrinking back below the small threshold");
    for (let i = 0; i < 5; i++) m.set("s" + i, i);
    eq(m.get("s3"), 3, "lookups after the index was dropped");
    ok(m.has(69) && !m.has(0), "has after shrink");
}
{
    const m = new Map();
    m.set(0, "zero");
    eq(m.get(-0), "zero", "-0 finds +0");
    m.set(-0, "neg");
    eq([...m.keys()].map((k) => Object.is(k, -0)), [false], "-0 is stored as +0");
    m.set(NaN, "nan");
    eq(m.get(NaN), "nan", "NaN is a key");
    m.set(1, "int");
    eq(m.get(1.0), "int", "1.0 finds 1");
    m.set(2 ** 53, "big");
    eq(m.get(2 ** 53), "big", "a large double key");
    m.set(10n, "bigint");
    eq([m.get(10n), m.get(10)], ["bigint", undefined], "bigint and number keys are distinct");
    const sym = Symbol("s");
    m.set(sym, "sym");
    eq(m.get(sym), "sym", "symbol key");
    m.set(true, "t");
    m.set("true", "st");
    eq([m.get(true), m.get("true")], ["t", "st"], "boolean and string keys are distinct");
    m.set(null, "null");
    m.set(undefined, "undef");
    eq([m.get(null), m.get(undefined), m.has(void 0)], ["null", "undef", true], "null and undefined keys");
}
{
    const m = new Map();
    for (let i = 0; i < 200; i++) m.set("key" + i, i);
    let s = "key1";
    eq(m.get(s), 1, "lookup caches the hash on the probe string");
    s += "5";
    eq(m.get(s), 15, "an in-place append invalidates the cached hash");
    s += "0";
    eq(m.get(s), 150, "a second in-place append");
    const am = new Map();
    for (let k = 1; k <= 200; k++) am.set("a".repeat(k), k);
    let grown = "";
    let stale = 0;
    for (let k = 1; k <= 200; k++) {
        grown += "a";
        if (am.get(grown) !== k) stale++;
    }
    eq(stale, 0, "a string grown in place by += is found at every length (the cached hash is cleared)");
    let t = "key";
    t += "199";
    eq(m.get(t), 199, "a concatenated probe");
    const obj = {};
    const k = "key" + 77;
    eq(m.get(k), 77, "before the string becomes a property name");
    obj[k] = 1;
    eq(m.get(k), 77, "after the string was interned as a property name");
    const fresh = JSON.parse('["key42"]')[0];
    eq(m.get(fresh), 42, "a string from JSON.parse");
    eq(m.get("key" + "8"), 8, "a constant-folded probe");
    const wide = new Map([["\u{1F600}x", 1], ["é", 2]]);
    eq([wide.get("\u{1F600}" + "x"), wide.get("é")], [1, 2], "wide-char keys");
    const sliceSrc = "prefix-" + "a".repeat(300) + "-suffix";
    const slice = sliceSrc.slice(7, 307);
    const sm = new Map([["a".repeat(300), "sliced"]]);
    eq(sm.get(slice), "sliced", "a sliced string finds the flat key");
    const rope = "b".repeat(600) + "c".repeat(600);
    const rm = new Map([["b".repeat(600) + "c".repeat(600), "rope"]]);
    eq(rm.get(rope), "rope", "a rope finds the flat key");
}
{
    const wm = new WeakMap();
    const keep = [];
    for (let i = 0; i < 300; i++) {
        const o = { i };
        wm.set(o, i);
        if (i % 3 === 0) keep.push(o);
    }
    gc();
    ok(keep.every((o) => wm.get(o) === o.i), "WeakMap keeps live keys through a collection");
    for (let i = 0; i < 300; i++) wm.set({}, i);
    gc();
    ok(keep.every((o) => wm.has(o)), "WeakMap after more garbage and a collection");
    keep.length = 0;
    gc();
    const fresh = {};
    wm.set(fresh, 1);
    eq(wm.get(fresh), 1, "WeakMap works after all keys died");
    const ws = new WeakSet();
    const ks = [{}, {}, {}];
    ks.forEach((o) => ws.add(o));
    ks.pop();
    gc();
    ok(ws.has(ks[0]) && ws.has(ks[1]), "WeakSet keeps live keys");
    const sym = Symbol("w");
    wm.set(sym, "symkey");
    gc();
    eq(wm.get(sym), "symkey", "a symbol WeakMap key");
}
{
    const big = new Map();
    for (let i = 0; i < 100000; i++) big.set(i * 7919 % 100003, i);
    ok(big.size === 100000, "100000 distinct keys");
    let okAll = true;
    for (let i = 0; i < 100000; i += 997) if (big.get(i * 7919 % 100003) !== i) okAll = false;
    ok(okAll, "100000-entry lookups");
    for (let i = 0; i < 100000; i += 2) big.delete(i * 7919 % 100003);
    ok(big.size === 50000 && !big.has(0) && big.has(7919 % 100003), "half deleted");
    let sum = 0;
    for (const v of big.values()) sum += v;
    eq(sum, 2500000000, "sum over the survivors in order");
    const iters = [big.keys(), big.keys(), big.keys()];
    iters[1].next();
    for (let i = 1; i < 100000; i += 2) big.delete(i * 7919 % 100003);
    eq(big.size, 0, "everything deleted");
    big.set("z", 1);
    eq(iters.map((it) => [...it]), [["z"], ["z"], ["z"]], "three live iterators after delete-all see the new entry");
}
{
    const s = new Set([1, 2, 3, 4, 5]);
    eq([...s.union(new Set([6, 1]))], [1, 2, 3, 4, 5, 6], "union");
    eq([...s.intersection(new Set([5, 1, 9]))].sort(), [1, 5], "intersection");
    eq([...s.difference(new Set([2, 3]))], [1, 4, 5], "difference");
    eq([...s.symmetricDifference(new Set([5, 6]))], [1, 2, 3, 4, 6], "symmetricDifference");
    ok(s.isSupersetOf(new Set([1, 2])) && new Set([0]).isSupersetOf(new Set([-0])), "isSupersetOf with -0");
    ok(new Set([1]).isSubsetOf(s) && s.isDisjointFrom(new Set([9])), "isSubsetOf / isDisjointFrom");
    const mut = new Set([1, 2, 3, 4]);
    const other = { size: 10, has(v) { mut.delete(v + 1); return true; }, keys() { return [].values(); } };
    eq([...mut.intersection(other)], [1, 3], "intersection while the callback deletes ahead");
}
{
    const m = new Map([[1, "a"], [2, "b"]]);
    const e = [...m.entries()];
    e[0][1] = "changed";
    eq(m.get(1), "a", "entries() returns fresh arrays");
    const from = new Map([[1, 2], [3, 4]].values());
    eq([...from], [[1, 2], [3, 4]], "new Map from an iterator of pairs");
    let calls = 0;
    class M extends Map { set(k, v) { calls++; return super.set(k, v); } }
    new M([[1, 1], [2, 2]]);
    eq(calls, 2, "a subclass set() is called by the constructor");
    const proto = Map.prototype.set;
    let protoCalls = 0;
    Map.prototype.set = function (k, v) { protoCalls++; return proto.call(this, k, v); };
    try { new Map([[1, 1]]); } finally { Map.prototype.set = proto; }
    eq(protoCalls, 1, "a replaced Map.prototype.set is called by the constructor");
    let getterHits = 0;
    const entry = [1, 2];
    Object.defineProperty(entry, 0, { get() { getterHits++; return "k"; } });
    const gm = new Map([entry]);
    eq([getterHits, gm.get("k")], [1, 2], "an entry with an accessor goes through [[Get]]");
    let threw = null;
    try { new Map([1]); } catch (x) { threw = x; }
    ok(threw instanceof TypeError, "a non-object entry throws TypeError");
    eq([...new Map([[1, 2, 3]])], [[1, 2]], "extra entry elements are ignored");
    eq([...new Map([[1]])], [[1, undefined]], "a one-element entry gives an undefined value");
}

print(`map internals: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failures");
