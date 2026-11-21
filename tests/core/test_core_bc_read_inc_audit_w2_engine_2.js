// covers: c:src/serialize/bc_read.inc.c
// tests/core/test_audit_w2_engine_2.js -- value-level regressions for the engine
// built-ins audited in wave 2. Every expected value is either a definition
// (a naive reference computed here) or a property stated in dynajs.d.ts.
//   E5-03  Object.set never walks or writes through the prototype chain
//   E5-04  assocPath/dissocPath/modifyPath refuse a runaway path cleanly
//   E5-06  keysIn/valuesIn/omit keep their answers (and stop being quadratic)
//   E5-07  once/debounce state is not observable through Array.prototype
//   E2c-02 BigInt64Array out-of-range store with an int key throws like the
//          string-key path
//   E3-06  indexOf/lastIndexOf/includes/count/split agree with a naive
//          reference on inputs that force the linear-time fallback
//   E3-07  Float32Array sum/mean accumulate in double
//   E3-09  Number.format keeps non-ASCII separators intact
//   E3-10  String.format reads own properties only
//   E2b-01 colliding keys do not make interning quadratic
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_audit_w2_engine_2");
import { cpuNow } from "../lib/cpuclock.js";
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    ok(threw, msg);
}

// ---- E5-03 ---------------------------------------------------------------

__kitS.section("Object.set __proto__ path does not pollute", () => {
    Object.set({}, "__proto__.polluted_w2", "yes");
    eq(({}).polluted_w2, undefined, "Object.set __proto__ path does not pollute");
    Object.set({}, ["constructor", "prototype", "polluted_w2b"], 1);
    eq(({}).polluted_w2b, undefined, "Object.set constructor.prototype path does not pollute");
    const o = Object.set({}, "__proto__.x", 1);
    ok(Object.hasOwn(o, "__proto__"), "Object.set creates an own __proto__ key");
    eq(Object.getPrototypeOf(o), Object.prototype, "Object.set leaves the prototype alone");
    const t = { a: { b: 1 } };
    Object.set(t, "a.c.d", 5);
    eq(t.a.c.d, 5, "Object.set creates missing hops");
    eq(t.a.b, 1, "Object.set keeps siblings");
    const arr = { list: [1, 2, 3] };
    Object.set(arr, "list.1", 9);
    eq(arr.list[1], 9, "Object.set writes an own array index");
    let hit = 0;
    const acc = { set v(x) { hit = x; }, get v() { return 0; } };
    Object.set(acc, "v", 7);
    eq(hit, 7, "Object.set still runs an OWN setter on the leaf");
    const lensed = Lens.prop("__proto__").set({ evil: 1 }, {});
    eq(Object.getPrototypeOf(lensed), Object.prototype, "Lens key set defines, never re-parents");
});

__kitS.section("assocPath runaway path throws", () => {
    const deep = new Array(2000000).fill("a").join(".");
    let name = "";
    try { Object.assocPath(deep, 1, {}); } catch (e) { name = e.name; }
    ok(name === "RangeError" || name === "InternalError", "assocPath runaway path throws (got " + name + ")");
    eq(Object.assocPath(["a", "b"], 1, {}).a.b, 1, "assocPath still works");
    eq(Object.dissocPath(["a", "b"], { a: { b: 1, c: 2 } }).a.c, 2, "dissocPath still works");
});

__kitS.section("keysIn order and cross-level dedupe", () => {
    const base = { p1: 1, shared: "base" };
    const mid = Object.create(base); mid.p2 = 2; mid.shared = "mid";
    const top = Object.create(mid); top.p3 = 3; top.shared = "top";
    eq(Object.keysIn(top).join(","), "p3,shared,p2,p1", "keysIn order and cross-level dedupe");
    eq(Object.valuesIn(top).join(","), "3,top,2,1", "valuesIn reads the shadowing value");
    const big = {};
    for (let i = 0; i < 30000; i++) big["k" + i] = i;
    const kin = Object.keysIn(big);
    eq(kin.length, 30000, "keysIn keeps every own key");
    eq(kin[29999], "k29999", "keysIn keeps insertion order");
    const drop = [];
    for (let i = 0; i < 30000; i += 2) drop.push("k" + i);
    const om = Object.omit(drop, big);
    const omk = Object.keys(om);
    eq(omk.length, 15000, "omit drops exactly the listed keys");
    ok(omk.every((k) => k[0] === "k" && (+k.slice(1)) % 2 === 1), "omit keeps only the unlisted keys");
    eq(om.k29999, 29999, "omit keeps the values of the surviving keys");
    eq(Object.keys(Object.omit(["b", "zz", "b"], { a: 1, b: 2, c: 3 })).join(","), "a,c", "omit small list with duplicates and misses");
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_engine_2 failed");
}
print("test_audit_w2_engine_2: " + checks + " checks passed");
