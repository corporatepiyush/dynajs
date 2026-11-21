// covers: dyna:json.Pointer.unescape
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_pointer_unescape_audit");
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

__kitS.section("unescape requires a string", () => {
throws(() => Pointer.unescape(null), "unescape requires a string");
});

__kitS.section("'~~' is refused (bare tilde); the engine is stricter than the RFC's two-pass", () => {
throws(() => Pointer.unescape("~~0"),
    "'~~' is refused (bare tilde); the engine is stricter than the RFC's two-pass");
});

__kitS.section("a dangling ~ refuses", () => {
throws(() => Pointer.unescape("a~"), "a dangling ~ refuses");
});

__kitS.section("unescape '~0~1' is '~/'", () => {
assert(Pointer.unescape("~0~1") === "~/", "unescape '~0~1' is '~/'");
});

__kitS.section("lone surrogates survive escape/unescape", () => {
assert(Pointer.unescape(Pointer.escape("\uDC00")) === "\uDC00",
    "lone surrogates survive escape/unescape");
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
