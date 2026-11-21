// covers: dyna:json.Pointer.remove
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_pointer_remove_audit");
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

throws(() => Pointer.remove([1, 2], "/2"), "remove idx==len refuses");

throws(() => Pointer.remove([1], "/-"), "remove '-' refuses");

__kitS.section("remove shifts mid-array in place", () => {
    const d = {list: [1, 2, 3]};
    const r = Pointer.remove(d, "/list/1");
    assert(r === d && deepEq(d.list, [1, 3]), "remove shifts mid-array in place");
    throws(() => Pointer.remove({}, ""), "remove root refuses (message names the root)");
});

__kitS.section("Pointer.remove: cannot remove the document root", () => {
    let m = "";
    try { Pointer.remove({}, ""); } catch (e) { m = String(e.message); }
    assert(m === "Pointer.remove: cannot remove the document root",
        "remove-root message names the function once (no 'remove: remove'): " + m);
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
