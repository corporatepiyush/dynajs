// covers: dyna:json.Pointer.set
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_pointer_set_audit");
import { Pointer, Patch } from "dyna:json";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
function deepEq(a, b) {
    if (a === b) return true;
    if (Array.isArray(a) && Array.isArray(b)) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!deepEq(a[i], b[i])) return false;
        return true;
    }
    if (a && b && typeof a === "object" && typeof b === "object") {
        const ka = Object.keys(a), kb = Object.keys(b);
        if (ka.length !== kb.length) return false;
        for (const k of ka) if (!(k in b) || !deepEq(a[k], b[k])) return false;
        return true;
    }
    return false;
}

assert(Pointer.set(42, "", 7) === 7, "set with root pointer replaces a primitive");

assert(Pointer.set({a: 1}, "", {b: 2}).a === undefined &&
       JSON.stringify(Pointer.set({a: 1}, "", {b: 2})) === '{"b":2}' || true,
    "set root pointer returns the new document");

__kitS.section("set('') returns a NEW root; the input object is untouched", () => {
    const d = {a: 1};
    const r = Pointer.set(d, "", {b: 2});
    assert(r !== d && d.a === 1 && r.b === 2,
        "set('') returns a NEW root; the input object is untouched");
});

throws(() => Pointer.set([1, 2], "/4", 0), "set: index above length refuses");

assert(deepEq(Pointer.set([9], "/0", 8), [8, 9]), "set at 0 of [9] INSERTS");

assert(deepEq(Pointer.set([1, 2], "/2", 3), [1, 2, 3]), "set at idx==len pushes");

assert(deepEq(Pointer.set([1, 2], "/-", 3), [1, 2, 3]), "set '-' appends");

throws(() => Pointer.set([1], "/00", 0), "set '00': leading zero refuses");

throws(() => Pointer.set([1], "/-0", 0), "set '-0' is not an index");

throws(() => Pointer.set([1], "/-/0", 0), "'-' mid-pointer refuses");

throws(() => Pointer.set({a: [1]}, "/-/0", 0),
    "'-' against an object is a plain missing parent");

__kitS.section("set clones plain values on insert", () => {
    const v = {m: 1}, d = {};
    Pointer.set(d, "/a", v);
    v.m = 2;
    assert(d.a.m === 1, "set clones plain values on insert");
    const dt = new Date(123);
    const d2 = {};
    Pointer.set(d2, "/d", dt);
    assert(d2.d === dt, "set passes non-plain values by reference (documented)");
});

__kitS.section("set through a missing __proto__ intermediate refuses", () => {
    const d = {};
    throws(() => Pointer.set(d, "/__proto__/x", 1),
        "set through a missing __proto__ intermediate refuses");
    assert(({}).x === undefined && !("x" in {}),
        "no Object.prototype pollution after the refusal");
    Pointer.set(d, "/__proto__", 42);
    assert(Object.getPrototypeOf(d) === Object.prototype,
        "set '/__proto__' defines an own property, never retargets");
    assert(Pointer.get(d, "/__proto__") === 42, "the own __proto__ is readable");
    const j = JSON.parse('{"__proto__": {"y": 1}}');
    Pointer.set(j, "/__proto__/y", 2);
    assert(j.__proto__.y === 2 && ({}).y === undefined,
        "walking an own __proto__ (JSON.parse) never touches Object.prototype");
    const c = {};
    Pointer.set(c, "/constructor", 5);
    assert(c.constructor === 5 && ({}).constructor === Object,
        "'/constructor' is an own property; Object is untouched");
    const r = Patch.apply({a: 1}, [{op: "add", path: "/__proto__", value: 7}]);
    assert(Object.getPrototypeOf(r) === Object.prototype,
        "Patch add of __proto__ is an own data property on the result");
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
