// covers: dyna:json.Pointer.has
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_pointer_has_audit");
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

__kitS.section("has: trailing slash on a leaf is false", () => {
assert(Pointer.has({a: 1}, "/a/") === false,
    "has: trailing slash on a leaf is false");
});

__kitS.section("has: the root of a primitive exists", () => {
assert(Pointer.has(42, "") === true, "has: the root of a primitive exists");
});

__kitS.section("has: no members under a primitive", () => {
assert(Pointer.has(42, "/x") === false, "has: no members under a primitive");
});

__kitS.section("has: '-' never resolves (absence)", () => {
assert(Pointer.has([1], "/-") === false, "has: '-' never resolves (absence)");
});

function deepPtr(m) { let p = ""; for (let i = 0; i < m; i++) p += "/0"; return p; }

function nest(d) { let v = 9; for (let i = 0; i < d; i++) v = [v]; return v; }

__kitS.section("has: the depth cap throws (syntax class), never false", () => {
throws(() => Pointer.has(nest(129), deepPtr(129)),
    "has: the depth cap throws (syntax class), never false");
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
