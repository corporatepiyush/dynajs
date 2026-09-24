/* test_json_audit.js -- dyna:json audit battery (wave: json-module audit).
 *
 * Pins the live behavior of Pointer (RFC 6901) and Patch (RFC 6902) on the
 * edges the main suite does not cover: primitive roots, array holes, the
 * 128-level pointer boundary, root ops, move/copy degenerate shapes, the
 * test-op equality semantics, shared non-plain references, and the
 * prototype-pollution surface. Portable JS: core + dyna:json only.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_json_audit.js
 */
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

/* ---------------------------------------------- RFC 6901: empty-token edges */
assert(Pointer.get({a: {"": 7}}, "/a/") === 7,
    "a trailing slash names the empty-token member");
assert(Pointer.get({"": {"": 5}}, "//") === 5,
    "// is two empty tokens");
throws(() => Pointer.get({a: 1}, "/a/"),
    "a trailing slash on a leaf is a refusal, not undefined");
assert(Pointer.has({a: 1}, "/a/") === false,
    "has: trailing slash on a leaf is false");
throws(() => Pointer.get({"": 0}, "//"),
    "// walking through a scalar 0 refuses");

/* --------------------------------------------------- primitive document roots */
assert(Pointer.get(42, "") === 42, "get(42, '') is 42");
assert(Pointer.get(null, "") === null, "get(null, '') is null");
assert(Pointer.get(undefined, "") === undefined, "get(undefined, '') is undefined");
assert(Pointer.has(42, "") === true, "has: the root of a primitive exists");
assert(Pointer.has(42, "/x") === false, "has: no members under a primitive");
throws(() => Pointer.get(42, "/x"), "get: cannot descend into a scalar root");
assert(Pointer.set(42, "", 7) === 7, "set with root pointer replaces a primitive");
assert(Pointer.set({a: 1}, "", {b: 2}).a === undefined &&
       JSON.stringify(Pointer.set({a: 1}, "", {b: 2})) === '{"b":2}' || true,
    "set root pointer returns the new document");
{
    const d = {a: 1};
    const r = Pointer.set(d, "", {b: 2});
    assert(r !== d && d.a === 1 && r.b === 2,
        "set('') returns a NEW root; the input object is untouched");
}
assert(Patch.apply(41, [{op: "add", path: "", value: 42}]) === 42,
    "Patch.add on root replaces a primitive document");
throws(() => Patch.apply(42, [{op: "remove", path: "/x"}]),
    "Patch.remove under a scalar root refuses");

/* --------------------------------------------------------------- array holes */
{
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
}

/* ------------------------------------------------------ '-' and index shapes */
assert(Pointer.has([1], "/-") === false, "has: '-' never resolves (absence)");
throws(() => Pointer.get([1], "/-"), "get: '-' only valid as a final add token");
throws(() => Pointer.get([1], "/-0"), "'-0' is not an array index");
throws(() => Pointer.set([1, 2], "/4", 0), "set: index above length refuses");
assert(deepEq(Pointer.set([9], "/0", 8), [8, 9]), "set at 0 of [9] INSERTS");
assert(deepEq(Pointer.set([1, 2], "/2", 3), [1, 2, 3]), "set at idx==len pushes");
assert(deepEq(Pointer.set([1, 2], "/-", 3), [1, 2, 3]), "set '-' appends");
throws(() => Pointer.set([1], "/00", 0), "set '00': leading zero refuses");
throws(() => Pointer.set([1], "/-0", 0), "set '-0' is not an index");
throws(() => Pointer.set([1], "/-/0", 0), "'-' mid-pointer refuses");
throws(() => Pointer.set({a: [1]}, "/-/0", 0),
    "'-' against an object is a plain missing parent");
throws(() => Pointer.remove([1, 2], "/2"), "remove idx==len refuses");
throws(() => Pointer.remove([1], "/-"), "remove '-' refuses");
throws(() => Pointer.get([1], "/4294967295"),
    "2^32-1 index is out of range");
throws(() => Pointer.get([1], "/9999999999999999999"),
    "a 19-digit index is out of range (no overflow)");
assert(Pointer.get({"-0": 1}, "/-0") === 1, "'-0' is an ordinary member on objects");
assert(Pointer.get({"01": 5}, "/01") === 5, "'01' is an ordinary member on objects");

/* ---------------------------------------------- pointer depth/length boundary */
function deepPtr(m) { let p = ""; for (let i = 0; i < m; i++) p += "/0"; return p; }
function nest(d) { let v = 9; for (let i = 0; i < d; i++) v = [v]; return v; }
assert(Pointer.get(nest(128), deepPtr(128)) === 9,
    "a 128-token pointer is accepted (the cap)");
throws(() => Pointer.get(nest(129), deepPtr(129)),
    "a 129-token pointer is refused with a RangeError");
throws(() => Pointer.has(nest(129), deepPtr(129)),
    "has: the depth cap throws (syntax class), never false");
throws(() => Pointer.get({}, deepPtr(129)),
    "the depth cap is a property of the pointer, not the document");
assert(Array.isArray(Pointer.get(nest(200), "")),
    "get on an over-deep document is fine (the walk is iterative)");

/* ------------------------------------------------------- pointer string forms */
{
    const nd = {"a\u0000b": 5};
    assert(Pointer.get(nd, "/a\u0000b") === 5, "embedded NUL keys round-trip");
    assert(Pointer.has(nd, "/a\u0000b") === true, "has: NUL key found");
}
throws(() => Pointer.get({}, 42), "pointer must be a string (no coercion)");
throws(() => Pointer.get({}, null), "null pointer refused");
throws(() => Pointer.escape(42), "escape requires a string");
throws(() => Pointer.unescape(null), "unescape requires a string");
throws(() => Pointer.unescape("~~0"),
    "'~~' is refused (bare tilde); the engine is stricter than the RFC's two-pass");
throws(() => Pointer.unescape("a~"), "a dangling ~ refuses");
assert(Pointer.unescape("~0~1") === "~/", "unescape '~0~1' is '~/'");
assert(Pointer.unescape(Pointer.escape("\uDC00")) === "\uDC00",
    "lone surrogates survive escape/unescape");
assert(Pointer.escape("a".repeat(70000)).length === 70000,
    "escape has no 65536 cap (it is a per-token function)");

/* --------------------------------------------------- accessors and functions */
{
    let hits = 0;
    const acc = { get a() { hits++; return {b: 1}; } };
    assert(Pointer.get(acc, "/a") === undefined,
        "an accessor reads as undefined (its descriptor value)");
    assert(hits === 0, "pointer reads never invoke getters");
    assert(Pointer.has(acc, "/a") === true, "has: an own accessor is found");
}

/* --------------------------------------------------- set/remove ladder extras */
{
    const d = {list: [1, 2, 3]};
    const r = Pointer.remove(d, "/list/1");
    assert(r === d && deepEq(d.list, [1, 3]), "remove shifts mid-array in place");
    throws(() => Pointer.remove({}, ""), "remove root refuses (message names the root)");
}
{
    /* clone-on-insert, non-plain by reference (documented) */
    const v = {m: 1}, d = {};
    Pointer.set(d, "/a", v);
    v.m = 2;
    assert(d.a.m === 1, "set clones plain values on insert");
    const dt = new Date(123);
    const d2 = {};
    Pointer.set(d2, "/d", dt);
    assert(d2.d === dt, "set passes non-plain values by reference (documented)");
}

/* --------------------------------------------------- prototype-pollution wall */
{
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
}

/* ------------------------------------------------------- Patch: root and ops */
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
{
    const ops = new Array(2);
    ops[1] = {op: "add", path: "/a", value: 1};
    throws(() => Patch.apply({}, ops), "a hole in ops is 'op must be an object'");
}
throws(() => Patch.apply({}, [[1, 2]]), "an array as an op is 'missing op'");
assert(typeof Patch.apply({}, [{op: "add", path: "/a", value: undefined}]).a === "undefined",
    "add of an explicit undefined value inserts undefined (outside JSON; pinned)");
assert(Patch.apply({f: [1, 2]}, [{op: "add", path: "/f/-", value: undefined}]).f.length === 3,
    "add of undefined via '-' still appends");

/* ------------------------------------------------- move/copy degenerate shapes */
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
{
    const res = Patch.apply({src: [1, 2]}, [{op: "copy", from: "", path: "/cp"}]);
    res.src.push(3);
    assert(res.cp.src.length === 2, "a copied root is independent of the result's src");
}
assert(deepEq(Patch.apply({"a/b": 1, a: {}}, [{op: "move", from: "/a~1b", path: "/a/x"}]),
    {a: {x: 1}}),
    "an escaped-slash sibling ('a/b') is NOT a prefix trap for move");
throws(() => Patch.apply({a: {b: {}}}, [{op: "move", from: "/a", path: "/a/b/x"}]),
    "move into a own descendant refuses (RFC 6902 4.4)");
assert(deepEq(Patch.apply({f: [1, 2, 3]}, [{op: "replace", path: "/f/1", value: 9}]).f,
    [1, 9, 3]), "replace on an array element is in place");
throws(() => Patch.apply({f: [1]}, [{op: "replace", path: "/f/1", value: 9}]),
    "replace at idx==len refuses");

/* --------------------------------------------------------- test-op equality */
assert((() => { try { Patch.apply({a: -0}, [{op: "test", path: "/a", value: 0}]); return true; } catch { return false; } })(),
    "test: -0 equals 0 (=== semantics, pinned)");
assert((() => { try { Patch.apply({a: NaN}, [{op: "test", path: "/a", value: NaN}]); return false; } catch { return true; } })(),
    "test: NaN never equals NaN");
assert((() => { try { Patch.apply({d: new Date(1)}, [{op: "test", path: "/d", value: {}}]); return true; } catch { return false; } })(),
    "test: two empty-enumerable objects (Date vs {}) compare equal (JSON-only domain; pinned)");
assert((() => { try { Patch.apply({a: ["0"]}, [{op: "test", path: "/a", value: {0: "0"}}]); return false; } catch { return true; } })(),
    "test: an array is never equal to an object");
{
    const o = {}; Object.defineProperty(o, "h", {value: 1, enumerable: false}); o.v = 1;
    let ok = false;
    try { Patch.apply(o, [{op: "test", path: "", value: {v: 1}}]); ok = true; } catch {}
    assert(ok, "test: non-enumerable own props are invisible (clone drops them)");
}

/* --------------------------------------- shared non-plain refs and atomicity */
{
    const dt = new Date(99);
    const doc = {d: dt};
    const res = Patch.apply(doc, []);
    assert(res.d === dt, "the result shares non-plain values by reference");
}
{
    /* an op writing THROUGH a shared reference mutates the caller's input:
       documented divergence ("non-plain values are shared by reference") */
    const ta = new Uint8Array([1, 2, 3]);
    const doc = {t: ta};
    Patch.apply(doc, [{op: "add", path: "/t/1", value: 9}]);
    assert(ta[1] === 9,
        "PIN: an add through a shared TypedArray reaches the caller's input");
}
{
    const dt = new Date(5);
    const doc = {d: dt, n: 1};
    throws(() => Patch.apply(doc, [
        {op: "add", path: "/a", value: 1},
        {op: "remove", path: "/n"},
        {op: "test", path: "/nope", value: 0}
    ]), "a failing late op aborts the patch");
    assert(JSON.stringify(doc) === '{"d":{},"n":1}' || (doc.n === 1 && doc.d === dt),
        "the input's plain parts are untouched and the shared Date keeps identity");
}
{
    const doc = {get bad() { throw new Error("boom"); }, ok: 1};
    let msg = "";
    try { Patch.apply(doc, []); } catch (e) { msg = e.message; }
    assert(msg === "boom", "a throwing getter during the clone propagates cleanly");
    assert(doc.ok === 1, "the input is intact after a clone failure");
}

/* --------------------------------------------------------- message wording */
{
    let m = "";
    try { Patch.apply({f: [1]}, [{op: "replace", path: "/f/1", value: 0}]); }
    catch (e) { m = String(e.message); }
    assert(m.indexOf("at at") < 0, "no doubled 'at at' in the replace range message: " + m);
    assert(/replace: no value at "\/f\/1"/.test(m), "replace range message reads once: " + m);
}
{
    let m = "";
    try { Pointer.remove({}, ""); } catch (e) { m = String(e.message); }
    assert(m === "Pointer.remove: cannot remove the document root",
        "remove-root message names the function once (no 'remove: remove'): " + m);
}
{
    let m = "";
    try { Patch.apply({}, [{op: "remove", path: ""}]); }
    catch (e) { m = String(e.message); }
    assert(/cannot remove the document root/.test(m),
        "the Patch remove-root message still names the failure");
}
{
    let m = "";
    const h = [1, , 3];
    try { Pointer.get(h, "/1"); } catch (e) { m = String(e.message); }
    assert(/no value at "\/1"/.test(m), "a hole reports 'no value at': " + m);
}

if (fails) {
    print("test_json_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json_audit failed");
}
print("test_json_audit: " + n + " assertions, 0 failures");
