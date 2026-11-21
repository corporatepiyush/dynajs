// covers: dyna:json.Patch.apply
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_patch_apply_audit");
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

assert(Patch.apply(41, [{op: "add", path: "", value: 42}]) === 42,
    "Patch.add on root replaces a primitive document");

throws(() => Patch.apply(42, [{op: "remove", path: "/x"}]),
    "Patch.remove under a scalar root refuses");

assert(deepEq(Patch.apply({a: 1}, [{op: "add", path: "", value: 7}]), 7),
    "Patch: add on root replaces the whole document");

assert(Patch.apply({a: 1}, [{op: "replace", path: "", value: 8}]) === 8,
    "Patch: replace on root replaces the whole document");

throws(() => Patch.apply({a: 1}, [{op: "remove", path: ""}]),
    "Patch: remove on root refuses");

assert(deepEq(Patch.apply({a: 1}, [{op: "test", path: "", value: {a: 1}}]), {a: 1}),
    "Patch: test on root with deep equality passes");

throws(() => Patch.apply({a: 1}, [{op: "test", path: "", value: {a: 2}}]),
    "Patch: test on root with a mismatch fails");

throws(() => Patch.apply({}, {length: 1, 0: {op: "add", path: "/a", value: 1}}),
    "an array-like ops object is refused (must be an Array)");

throws(() => Patch.apply({}, null), "null ops refused");

__kitS.section("a hole in ops is 'op must be an object'", () => {
    const ops = new Array(2);
    ops[1] = {op: "add", path: "/a", value: 1};
    throws(() => Patch.apply({}, ops), "a hole in ops is 'op must be an object'");
});

throws(() => Patch.apply({}, [[1, 2]]), "an array as an op is 'missing op'");

assert(typeof Patch.apply({}, [{op: "add", path: "/a", value: undefined}]).a === "undefined",
    "add of an explicit undefined value inserts undefined (outside JSON; pinned)");

assert(Patch.apply({f: [1, 2]}, [{op: "add", path: "/f/-", value: undefined}]).f.length === 3,
    "add of undefined via '-' still appends");

throws(() => Patch.apply({a: 1}, [{op: "move", from: "", path: "/b"}]),
    "move: the root is a proper prefix of any path -> refused");

throws(() => Patch.apply({a: 1}, [{op: "move", from: "", path: ""}]),
    "move '' to '' fails at 'cannot remove the document root'");

assert(deepEq(Patch.apply({a: {b: 1}}, [{op: "move", from: "/a/b", path: "/a/b"}]),
    {a: {b: 1}}), "move to the same object path is a value no-op");

assert(deepEq(Patch.apply({f: [1, 2, 3]}, [{op: "move", from: "/f/1", path: "/f/1"}]).f,
    [1, 2, 3]), "move of an array element onto itself is a no-op");

assert(Patch.apply({a: {b: {c: 1}}}, [{op: "move", from: "/a/b/c", path: "/c"}]).c === 1,
    "moving a child UP (from deeper than path) is legal");

assert(deepEq(Patch.apply({a: 1}, [{op: "copy", from: "", path: "/cp"}]),
    {a: 1, cp: {a: 1}}), "copy from the root duplicates the whole document");

__kitS.section("a copied root is independent of the result's src", () => {
    const res = Patch.apply({src: [1, 2]}, [{op: "copy", from: "", path: "/cp"}]);
    res.src.push(3);
    assert(res.cp.src.length === 2, "a copied root is independent of the result's src");
});

assert(deepEq(Patch.apply({"a/b": 1, a: {}}, [{op: "move", from: "/a~1b", path: "/a/x"}]),
    {a: {x: 1}}),
    "an escaped-slash sibling ('a/b') is NOT a prefix trap for move");

throws(() => Patch.apply({a: {b: {}}}, [{op: "move", from: "/a", path: "/a/b/x"}]),
    "move into a own descendant refuses (RFC 6902 4.4)");

assert(deepEq(Patch.apply({f: [1, 2, 3]}, [{op: "replace", path: "/f/1", value: 9}]).f,
    [1, 9, 3]), "replace on an array element is in place");

throws(() => Patch.apply({f: [1]}, [{op: "replace", path: "/f/1", value: 9}]),
    "replace at idx==len refuses");

assert((() => { try { Patch.apply({a: -0}, [{op: "test", path: "/a", value: 0}]); return true; } catch { return false; } })(),
    "test: -0 equals 0 (=== semantics, pinned)");

assert((() => { try { Patch.apply({a: NaN}, [{op: "test", path: "/a", value: NaN}]); return false; } catch { return true; } })(),
    "test: NaN never equals NaN");

assert((() => { try { Patch.apply({d: new Date(1)}, [{op: "test", path: "/d", value: {}}]); return true; } catch { return false; } })(),
    "test: two empty-enumerable objects (Date vs {}) compare equal (JSON-only domain; pinned)");

assert((() => { try { Patch.apply({a: ["0"]}, [{op: "test", path: "/a", value: {0: "0"}}]); return false; } catch { return true; } })(),
    "test: an array is never equal to an object");

__kitS.section("test: non-enumerable own props are invisible (clone drops them)", () => {
    const o = {}; Object.defineProperty(o, "h", {value: 1, enumerable: false}); o.v = 1;
    let ok = false;
    try { Patch.apply(o, [{op: "test", path: "", value: {v: 1}}]); ok = true; } catch {}
    assert(ok, "test: non-enumerable own props are invisible (clone drops them)");
});

__kitS.section("the result shares non-plain values by reference", () => {
    const dt = new Date(99);
    const doc = {d: dt};
    const res = Patch.apply(doc, []);
    assert(res.d === dt, "the result shares non-plain values by reference");
});

__kitS.section("PIN: an add through a shared TypedArray reaches the caller's input", () => {
    const ta = new Uint8Array([1, 2, 3]);
    const doc = {t: ta};
    Patch.apply(doc, [{op: "add", path: "/t/1", value: 9}]);
    assert(ta[1] === 9,
        "PIN: an add through a shared TypedArray reaches the caller's input");
});

__kitS.section("a failing late op aborts the patch", () => {
    const dt = new Date(5);
    const doc = {d: dt, n: 1};
    throws(() => Patch.apply(doc, [
        {op: "add", path: "/a", value: 1},
        {op: "remove", path: "/n"},
        {op: "test", path: "/nope", value: 0}
    ]), "a failing late op aborts the patch");
    assert(JSON.stringify(doc) === '{"d":{},"n":1}' || (doc.n === 1 && doc.d === dt),
        "the input's plain parts are untouched and the shared Date keeps identity");
});

__kitS.section("a throwing getter during the clone propagates cleanly", () => {
    const doc = {get bad() { throw new Error("boom"); }, ok: 1};
    let msg = "";
    try { Patch.apply(doc, []); } catch (e) { msg = e.message; }
    assert(msg === "boom", "a throwing getter during the clone propagates cleanly");
    assert(doc.ok === 1, "the input is intact after a clone failure");
});

__kitS.section("at at", () => {
    let m = "";
    try { Patch.apply({f: [1]}, [{op: "replace", path: "/f/1", value: 0}]); }
    catch (e) { m = String(e.message); }
    assert(m.indexOf("at at") < 0, "no doubled 'at at' in the replace range message: " + m);
    assert(/replace: no value at "\/f\/1"/.test(m), "replace range message reads once: " + m);
});

__kitS.section("the Patch remove-root message still names the failure", () => {
    let m = "";
    try { Patch.apply({}, [{op: "remove", path: ""}]); }
    catch (e) { m = String(e.message); }
    assert(/cannot remove the document root/.test(m),
        "the Patch remove-root message still names the failure");
});
__kitS.end();

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
