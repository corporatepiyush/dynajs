/* test_json.js -- RFC 6901 JSON Pointer + RFC 6902 JSON Patch (plan 3.17).
 *
 * RFC 6901 5 lists eleven pointers against one document; every one is here.
 * RFC 6902 appendix A.1-A.16 (A.13 needs duplicate object keys, which JS
 * cannot express) and the section-3 "An Example" six-op sequence are exact
 * vectors. Section 5's atomicity example is the load-bearing test: a failed
 * apply must leave the document BYTE-IDENTICAL.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_json.js
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

/* ------------------------------------------------ RFC 6901 5: the document */
const D5 = {
    "foo": ["bar", "baz"],
    "": 0,
    "a/b": 1,
    "c%d": 2,
    "e^f": 3,
    "g|h": 4,
    "i\\j": 5,
    "k\"l": 6,
    " ": 7,
    "m~n": 8
};
/* RFC 6901 5: pointer -> value. The fragment form (6) needs percent-decoding,
 * which is the URI layer's job; these are the JSON-string representations. */
assert(deepEq(Pointer.get(D5, ""), D5), "RFC 6901 5: \"\" is the whole doc");
assert(deepEq(Pointer.get(D5, "/foo"), ["bar", "baz"]), "RFC 6901 5: /foo");
assert(Pointer.get(D5, "/foo/0") === "bar", "RFC 6901 5: /foo/0");
assert(Pointer.get(D5, "/") === 0, "RFC 6901 5: / (the empty key)");
assert(Pointer.get(D5, "/a~1b") === 1, "RFC 6901 5: /a~1b");
assert(Pointer.get(D5, "/c%d") === 2, "RFC 6901 5: /c%d");
assert(Pointer.get(D5, "/e^f") === 3, "RFC 6901 5: /e^f");
assert(Pointer.get(D5, "/g|h") === 4, "RFC 6901 5: /g|h");
assert(Pointer.get(D5, "/i\\j") === 5, "RFC 6901 5: /i\\j");
assert(Pointer.get(D5, "/k\"l") === 6, "RFC 6901 5: /k\"l");
assert(Pointer.get(D5, "/ ") === 7, "RFC 6901 5: /<space>");
assert(Pointer.get(D5, "/m~0n") === 8, "RFC 6901 5: /m~0n");
/* A member key that LOOKS like an array index is still a member of an object. */
assert(Pointer.get({ "0": "zero" }, "/0") === "zero", "digit key on an object");

/* ------------------------------------------------------------- has/get/set */
assert(Pointer.has(D5, "/foo/0") === true, "has: present");
assert(Pointer.has(D5, "/nope") === false, "has: missing");
assert(Pointer.has({a: 5}, "/a/b") === false, "has: into a scalar is not there");
assert(Pointer.has({foo: [1, 2]}, "/foo/9") === false, "has: index out of range");
/* RFC 6901 4: a leading zero is a SYNTAX error, so has() throws like get(). */
throws(() => Pointer.has({foo: [1, 2]}, "/foo/01"), "has: leading zero is a syntax error");
throws(() => Pointer.has({}, "/a~2b"), "has: syntax errors still throw");
throws(() => Pointer.get({foo: [1, 2]}, "/foo/-"), "RFC 6901 4: '-' never resolves for get");
throws(() => Pointer.get({a: [1, 2]}, "/a/01"), "RFC 6901 4: leading zero '01' refused");
throws(() => Pointer.get({a: [1, 2]}, "/a/00"), "RFC 6901 4: leading zero '00' refused");
throws(() => Pointer.get({a: [1, 2]}, "/a/x"), "RFC 6901 4: non-numeric on an array");
throws(() => Pointer.get({a: 5}, "/a/b"), "RFC 6901 7: cannot descend into a scalar");
throws(() => Pointer.get({a: 1}, "/a/b"), "RFC 6901 7: missing member");
throws(() => Pointer.get({}, "/a~2b"), "RFC 6901 3: invalid ~ escape");
throws(() => Pointer.get({}, "/a~"), "RFC 6901 3: dangling ~");
throws(() => Pointer.get({}, "nope"), "RFC 6901 3: pointer needs a leading '/'");
throws(() => Pointer.get({}, "/" + "a".repeat(70000)),
    "pointer longer than 65536 bytes is refused");

{
    const sd = {foo: [1, 2]};
    assert(Pointer.set(sd, "/foo/-", 3) === sd, "set returns the same document");
    assert(deepEq(sd.foo, [1, 2, 3]), "set: '-' appends");
    Pointer.set(sd, "/bar", {x: 1});
    assert(deepEq(sd.bar, {x: 1}), "set: adds a member");
    Pointer.set(sd, "/bar/x", 2);
    assert(sd.bar.x === 2, "set: replaces an existing member");
    Pointer.remove(sd, "/foo/1");
    assert(deepEq(sd.foo, [1, 3]), "remove: array element, elements shift");
    Pointer.remove(sd, "/bar");
    assert(!("bar" in sd), "remove: member");
    throws(() => Pointer.set(sd, "/nope/y", 1), "set: missing parent");
    throws(() => Pointer.remove(sd, "/nope"), "remove: missing target");
    throws(() => Pointer.remove(sd, ""), "remove: root is refused");
}

/* ----------------------------------------------------- escape / unescape */
assert(Pointer.unescape("a~1b") === "a/b", "unescape ~1");
assert(Pointer.unescape("m~0n") === "m~n", "unescape ~0");
assert(Pointer.unescape("~01") === "~1", "RFC 6902 A.14: ~01 is ~1, never '/'");
assert(Pointer.unescape("a~01b") === "a~1b", "~01 then a literal 1");
assert(Pointer.escape("a/b") === "a~1b", "escape '/'");
assert(Pointer.escape("m~n") === "m~0n", "escape '~'");
assert(Pointer.escape("a/b~c") === "a~1b~0c", "escape both");
throws(() => Pointer.unescape("a~2b"), "unescape refuses ~2");
throws(() => Pointer.unescape("a~"), "unescape refuses a dangling ~");
throws(() => Pointer.unescape("x~"), "unescape refuses a trailing ~");
for (const t of ["", "a/b", "m~n", "c%d", " ", "k\"l", "i\\j", "~01", "x/y~z"])
    assert(Pointer.unescape(Pointer.escape(t)) === t,
        "escape/unescape round trip: " + JSON.stringify(t));

/* ------------------------------------------ RFC 6902 A.1 - A.16 (exact) */
assert(deepEq(Patch.apply({foo: "bar"}, [{op: "add", path: "/baz", value: "qux"}]),
    {baz: "qux", foo: "bar"}), "RFC 6902 A.1: adding an object member");
assert(deepEq(Patch.apply({foo: ["bar", "baz"]},
    [{op: "add", path: "/foo/1", value: "qux"}]),
    {foo: ["bar", "qux", "baz"]}), "RFC 6902 A.2: adding an array element");
assert(deepEq(Patch.apply({baz: "qux", foo: "bar"},
    [{op: "remove", path: "/baz"}]),
    {foo: "bar"}), "RFC 6902 A.3: removing an object member");
assert(deepEq(Patch.apply({foo: ["bar", "qux", "baz"]},
    [{op: "remove", path: "/foo/1"}]),
    {foo: ["bar", "baz"]}), "RFC 6902 A.4: removing an array element");
assert(deepEq(Patch.apply({baz: "qux", foo: "bar"},
    [{op: "replace", path: "/baz", value: "boo"}]),
    {baz: "boo", foo: "bar"}), "RFC 6902 A.5: replacing a value");
assert(deepEq(Patch.apply(
    {foo: {bar: "baz", waldo: "fred"}, qux: {corge: "grault"}},
    [{op: "move", from: "/foo/waldo", path: "/qux/thud"}]),
    {foo: {bar: "baz"}, qux: {corge: "grault", thud: "fred"}}),
    "RFC 6902 A.6: moving a value");
assert(deepEq(Patch.apply({foo: ["all", "grass", "cows", "eat"]},
    [{op: "move", from: "/foo/1", path: "/foo/3"}]),
    {foo: ["all", "cows", "eat", "grass"]}),
    "RFC 6902 A.7: moving an array element (remove then add semantics)");
{
    const a8 = {baz: "qux", foo: ["a", 2, "c"]};
    assert(deepEq(Patch.apply(a8, [
        {op: "test", path: "/baz", value: "qux"},
        {op: "test", path: "/foo/1", value: 2}
    ]), a8), "RFC 6902 A.8: tests succeed, doc unchanged");
}
throws(() => Patch.apply({baz: "qux"}, [{op: "test", path: "/baz", value: "bar"}]),
    "RFC 6902 A.9: testing a value: error");
assert(deepEq(Patch.apply({foo: "bar"},
    [{op: "add", path: "/child", value: {grandchild: {}}}]),
    {foo: "bar", child: {grandchild: {}}}),
    "RFC 6902 A.10: adding a nested member object");
assert(deepEq(Patch.apply({foo: "bar"},
    [{op: "add", path: "/baz", value: "qux", xyz: 123}]),
    {foo: "bar", baz: "qux"}),
    "RFC 6902 A.11: unrecognized members are ignored");
throws(() => Patch.apply({foo: "bar"},
    [{op: "add", path: "/baz/bat", value: "qux"}]),
    "RFC 6902 A.12: adding to a nonexistent target errors");
/* A.13 (duplicate "op" members) is not expressible in JS objects. */
assert(deepEq(Patch.apply({"/": 9, "~1": 10},
    [{op: "test", path: "/~01", value: 10}]), {"/": 9, "~1": 10}),
    "RFC 6902 A.14: ~ escape ordering (~01 must hit '~1')");
throws(() => Patch.apply({"/": 9, "~1": 10},
    [{op: "test", path: "/~01", value: "10"}]),
    "RFC 6902 A.15: string is not a number, test fails");
assert(deepEq(Patch.apply({foo: ["bar"]},
    [{op: "add", path: "/foo/-", value: ["abc", "def"]}]),
    {foo: ["bar", ["abc", "def"]]}),
    "RFC 6902 A.16: '-' appends the array value");

/* --------------------------------------- RFC 6902 3: the full six-op run */
{
    const ex = {"a": {"b": {"c": "foo"}}};
    const got = Patch.apply(ex, [
        {op: "test", path: "/a/b/c", value: "foo"},
        {op: "remove", path: "/a/b/c"},
        {op: "add", path: "/a/b/c", value: ["foo", "bar"]},
        {op: "replace", path: "/a/b/c", value: 42},
        {op: "move", from: "/a/b/c", path: "/a/b/d"},
        {op: "copy", from: "/a/b/d", path: "/a/b/e"}
    ]);
    assert(deepEq(got, {"a": {"b": {"d": 42, "e": 42}}}),
        "RFC 6902 3: the six-op example yields /a/b/{d,e}=42");
    assert(got !== ex, "apply returns a NEW document");
    assert(deepEq(ex, {"a": {"b": {"c": "foo"}}}),
        "the input document is untouched by a successful apply");
}

/* ------------------------------------------ RFC 6902 5: ATOMICITY (the key) */
{
    const adoc = {"a": {"b": {"c": "foo"}}};
    const before = JSON.stringify(adoc);
    throws(() => Patch.apply(adoc, [
        {op: "replace", path: "/a/b/c", value: 42},
        {op: "test", path: "/a/b/c", value: "C"}
    ]), "RFC 6902 5: the failing test aborts the patch");
    assert(JSON.stringify(adoc) === before,
        "RFC 6902 5: failed apply leaves the document byte-identical");
}
/* A mid-patch failure after earlier SUCCESSFUL mutations must also roll back. */
{
    const adoc = {"a": 1, "b": [1, 2, 3]};
    const before = JSON.stringify(adoc);
    throws(() => Patch.apply(adoc, [
        {op: "add", path: "/x", value: 1},
        {op: "remove", path: "/b/0"},
        {op: "move", from: "/b/0", path: "/b/0/child"},  /* prefix rule fires */
        {op: "add", path: "/y", value: 2}
    ]), "a mid-patch move error aborts after earlier mutations");
    assert(JSON.stringify(adoc) === before,
        "mid-patch failure still leaves the document byte-identical");
}
/* The result must not alias the caller's ops array. */
{
    const doc = {};
    const val = {nested: 1};
    const res = Patch.apply(doc, [{op: "add", path: "/a", value: val}]);
    val.nested = 99;
    assert(res.a.nested === 1, "an added value is cloned, never aliased");
    res.a.nested = 5;
    assert(val.nested === 99, "mutating the result does not leak into the input");
}
/* copy must not alias its from-location. */
{
    const doc = {src: {v: 1}};
    const res = Patch.apply(doc, [{op: "copy", from: "/src", path: "/dst"}]);
    res.src.v = 2;
    assert(res.dst.v === 1, "copy is a deep copy, from and dst stay independent");
}

/* ------------------------------------------------------ test-op equality */
{
    const tdoc = {a: {b: [1, 2, {c: "x"}]}, d: 5};
    Patch.apply(tdoc, [{op: "test", path: "/a", value: {b: [1, 2, {c: "x"}]}}]);
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/a", value: {b: [1, 2, {c: "y"}]}}]),
        "test: a deep mismatch fails");
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/a", value: {b: [1, 2]}}]),
        "test: array length mismatch fails");
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/a", value: {b: [1, 2, {c: "x"}], z: 1}}]),
        "test: extra key fails");
    Patch.apply(tdoc, [{op: "test", path: "/d", value: 5}]);
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/d", value: "5"}]),
        "RFC 6902 4.6: a number is not the string of that number");
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/d", value: 6}]),
        "test: wrong number fails");
    throws(() => Patch.apply(tdoc, [{op: "test", path: "/a/b", value: [1, "2", {c: "x"}]}]),
        "test: number vs string inside an array fails");
    /* Key order is irrelevant for object equality (RFC 6902 4.6). */
    Patch.apply({x: 1, y: {z: 2}}, [{op: "test", path: "", value: {y: {z: 2}, x: 1}}]);
}

/* ---------------------------------------------- adversarial op documents */
throws(() => Patch.apply({}, [{op: "splice", path: "/a", value: 1}]),
    "unknown op is refused");
throws(() => Patch.apply({}, [{op: 42, path: "/a"}]),
    "op must be a string");
throws(() => Patch.apply({}, [{path: "/a", value: 1}]),
    "missing op is refused");
throws(() => Patch.apply({}, [{op: "add", value: 1}]),
    "missing path is refused");
throws(() => Patch.apply({}, [{op: "add", path: 42, value: 1}]),
    "path must be a string");
throws(() => Patch.apply({}, [{op: "add", path: "/a"}]),
    "add requires value");
throws(() => Patch.apply({}, [{op: "replace", path: "/a"}]),
    "replace requires value");
throws(() => Patch.apply({}, [{op: "test", path: "/a"}]),
    "test requires value");
throws(() => Patch.apply({}, [{op: "move", path: "/a"}]),
    "move requires from");
throws(() => Patch.apply({}, [{op: "copy", path: "/a"}]),
    "copy requires from");
throws(() => Patch.apply({a: 1}, [{op: "move", from: "/b", path: "/c"}]),
    "move: from must exist");
throws(() => Patch.apply({a: 1}, [{op: "copy", from: "/b", path: "/c"}]),
    "copy: from must exist");
throws(() => Patch.apply({a: {b: 1}}, [{op: "move", from: "/a", path: "/a/b"}]),
    "RFC 6902 4.4: cannot move into a child");
throws(() => Patch.apply({a: {b: 1}}, [{op: "move", from: "/a/b", path: "/a/b/c"}]),
    "RFC 6902 4.4: from cannot be a prefix of path");
throws(() => Patch.apply({foo: [1, 2]}, [{op: "add", path: "/foo/3", value: 0}]),
    "RFC 6902 4.1: add index above length is refused");
throws(() => Patch.apply({foo: [1, 2]}, [{op: "add", path: "/foo/01", value: 0}]),
    "add: leading-zero index refused");
throws(() => Patch.apply({foo: [1, 2]}, [{op: "add", path: "/foo/-2", value: 0}]),
    "add: negative index refused");
throws(() => Patch.apply({a: 5}, [{op: "add", path: "/a/b", value: 1}]),
    "add into a scalar is refused");
throws(() => Patch.apply({a: 1}, [{op: "remove", path: "/a/b"}]),
    "remove: missing target refused");
throws(() => Patch.apply({a: 1}, [{op: "replace", path: "/a/b", value: 2}]),
    "replace: missing target refused");
throws(() => Patch.apply({a: 1}, [{op: "test", path: "/a/b", value: 1}]),
    "test: missing target refused");
throws(() => Patch.apply({foo: [1, 2]}, [{op: "remove", path: "/foo/-"}]),
    "remove: '-' is not a real index");
throws(() => Patch.apply({foo: [1, 2]}, [{op: "test", path: "/foo/-", value: 1}]),
    "test: '-' is not a real index");
throws(() => Patch.apply({}, [{op: "add", path: "/a~2b", value: 1}]),
    "apply: invalid ~ escape refused");
throws(() => Patch.apply({}, [{op: "add", path: "/a~", value: 1}]),
    "apply: dangling ~ refused");
throws(() => Patch.apply({}, [{op: "add", path: "nope", value: 1}]),
    "apply: pointer without a leading '/' refused");
throws(() => Patch.apply({}, [{op: "add", path: "/" + "a".repeat(70000), value: 1}]),
    "apply: over-long path refused");
throws(() => Patch.apply({}, 42), "ops must be an array");
throws(() => Patch.apply({}, [null]), "each op must be an object");
/* The failing op's INDEX and PATH must appear in the message. */
try {
    Patch.apply({a: 1}, [
        {op: "add", path: "/x", value: 1},
        {op: "test", path: "/a", value: 9},
        {op: "remove", path: "/nope"}
    ]);
    assert(false, "the mid-patch failure threw");
} catch (e) {
    const m = String(e);
    assert(m.indexOf("[1]") >= 0, "message carries the failing op index: " + m);
    assert(m.indexOf("/a") >= 0, "message carries the failing path: " + m);
}

/* ------------------------------------------------ depth cap: 127 / 128 / 129 */
function chain(d) { let v = {}; for (let i = 0; i < d; i++) v = [v]; return v; }
assert(Array.isArray(Patch.apply(chain(127), [])), "depth 127 clones");
assert(Array.isArray(Patch.apply(chain(128), [])), "depth 128 (the cap) clones");
throws(() => Patch.apply(chain(129), []), "depth 129 is refused (RangeError)");
assert(Array.isArray(Pointer.get(chain(129), "")),
    "Pointer.get still works on an over-deep document (iterative walk)");
throws(() => Patch.apply({}, [{op: "add", path: "/a", value: chain(129)}]),
    "an over-deep ADD VALUE is refused too");

/* ------------------------------------------------ JSON.parse stack overflow guard */
{
    const deepArray = "[".repeat(10000) + "0" + "]".repeat(10000);
    let threw = false;
    try {
        JSON.parse(deepArray);
    } catch (e) {
        threw = (e instanceof InternalError) || String(e).indexOf("stack overflow") >= 0;
    }
    assert(threw, "JSON.parse(10k-nested-array) throws stack overflow gracefully");

    const deepObj = '{"a":'.repeat(10000) + "0" + "}".repeat(10000);
    threw = false;
    try {
        JSON.parse(deepObj);
    } catch (e) {
        threw = (e instanceof InternalError) || String(e).indexOf("stack overflow") >= 0;
    }
    assert(threw, "JSON.parse(10k-nested-obj) throws stack overflow gracefully");
}

if (fails) {
    print("test_json: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json failed");
}
print("test_json: " + n + " assertions, 0 failures");
