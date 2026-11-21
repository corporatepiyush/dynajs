// covers: dyna:json.Pointer.get
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_pointer_get_audit");
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

assert(Pointer.get({a: {"": 7}}, "/a/") === 7,
    "a trailing slash names the empty-token member");

assert(Pointer.get({"": {"": 5}}, "//") === 5,
    "// is two empty tokens");

throws(() => Pointer.get({a: 1}, "/a/"),
    "a trailing slash on a leaf is a refusal, not undefined");

throws(() => Pointer.get({"": 0}, "//"),
    "// walking through a scalar 0 refuses");

assert(Pointer.get(42, "") === 42, "get(42, '') is 42");

assert(Pointer.get(null, "") === null, "get(null, '') is null");

assert(Pointer.get(undefined, "") === undefined, "get(undefined, '') is undefined");

throws(() => Pointer.get(42, "/x"), "get: cannot descend into a scalar root");

__kitS.section("a HOLE is a missing element: get throws (not undefined)", () => {
    const h = [1, , 3];
    throws(() => Pointer.get(h, "/1"),
        "a HOLE is a missing element: get throws (not undefined)");
    assert(Pointer.has(h, "/1") === false, "has: a hole is not present");
    assert(Pointer.get(h, "/2") === 3, "the real elements around a hole read fine");
    const ex = [1, undefined, 3];
    assert(Pointer.has(ex, "/1") === true,
        "an explicit undefined element EXISTS (get reads undefined)");
    assert(Pointer.get(ex, "/1") === undefined,
        "get of an explicit undefined element is undefined");
});

throws(() => Pointer.get([1], "/-"), "get: '-' only valid as a final add token");

throws(() => Pointer.get([1], "/-0"), "'-0' is not an array index");

throws(() => Pointer.get([1], "/4294967295"),
    "2^32-1 index is out of range");

throws(() => Pointer.get([1], "/9999999999999999999"),
    "a 19-digit index is out of range (no overflow)");

assert(Pointer.get({"-0": 1}, "/-0") === 1, "'-0' is an ordinary member on objects");

assert(Pointer.get({"01": 5}, "/01") === 5, "'01' is an ordinary member on objects");

function deepPtr(m) { let p = ""; for (let i = 0; i < m; i++) p += "/0"; return p; }

function nest(d) { let v = 9; for (let i = 0; i < d; i++) v = [v]; return v; }

assert(Pointer.get(nest(128), deepPtr(128)) === 9,
    "a 128-token pointer is accepted (the cap)");

throws(() => Pointer.get(nest(129), deepPtr(129)),
    "a 129-token pointer is refused with a RangeError");

throws(() => Pointer.get({}, deepPtr(129)),
    "the depth cap is a property of the pointer, not the document");

assert(Array.isArray(Pointer.get(nest(200), "")),
    "get on an over-deep document is fine (the walk is iterative)");

__kitS.section("embedded NUL keys round-trip", () => {
    const nd = {"a\u0000b": 5};
    assert(Pointer.get(nd, "/a\u0000b") === 5, "embedded NUL keys round-trip");
    assert(Pointer.has(nd, "/a\u0000b") === true, "has: NUL key found");
});

throws(() => Pointer.get({}, 42), "pointer must be a string (no coercion)");

throws(() => Pointer.get({}, null), "null pointer refused");

__kitS.section("an accessor reads as undefined (its descriptor value)", () => {
    let hits = 0;
    const acc = { get a() { hits++; return {b: 1}; } };
    assert(Pointer.get(acc, "/a") === undefined,
        "an accessor reads as undefined (its descriptor value)");
    assert(hits === 0, "pointer reads never invoke getters");
    assert(Pointer.has(acc, "/a") === true, "has: an own accessor is found");
});

__kitS.section("a hole reports 'no value at':", () => {
    let m = "";
    const h = [1, , 3];
    try { Pointer.get(h, "/1"); } catch (e) { m = String(e.message); }
    assert(/no value at "\/1"/.test(m), "a hole reports 'no value at': " + m);
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
