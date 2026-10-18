// Black-box contract test for dyna:json, generated from dynajs.d.ts lines 2849-2905. Engine sources not consulted.
// Table-driven: every expectation is a case row; each failure names its row.
import { Pointer, Patch, Ndjson } from "dyna:json";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg);
}

const clone = (v) => JSON.parse(JSON.stringify(v));
const DOC = { a: { b: ["first", "second"] } };

// ------------------------------------------------------------------
// Pointer (RFC 6901)
// ------------------------------------------------------------------

// d.ts: "Walks pointer"; dynajs.d.ts: '"" is the whole document', "~0/~1 unescape", pointer
// "longer than 65536 bytes or deeper than 128 levels throws". Rows: [label, doc, pointer, expected].
{
    let deepDoc = 0, okDoc = 0, deepPtr = "", okPtr = "";
    for (let i = 0; i < 130; i++) { deepDoc = { a: deepDoc }; deepPtr = "/a" + deepPtr; }
    for (let i = 0; i < 128; i++) { okDoc = { a: okDoc }; okPtr = "/a" + okPtr; }

    const GET_OK = [
        ["dynajs.d.ts example /a/b/1", DOC, "/a/b/1", "second"],
        ["array index 0", DOC, "/a/b/0", "first"],
        ["\"\" is the whole document", DOC, "", DOC],
        ["an empty token addresses the \"\" key (RFC 6901)", { "": 1 }, "/", 1],
        ["~1 -> / and ~0 -> ~ (RFC 6901)", { "a/b": { "c~d": 7 } }, "/a~1b/c~0d", 7],
        ["depth 128 is inside the cap (only \"deeper than 128\" throws)", okDoc, okPtr, 0],
        ["an object branch returns the sub-value", DOC, "/a", { b: ["first", "second"] }],
    ];
    for (const [label, doc, pointer, expected] of GET_OK) {
        if (typeof expected === "object") assertDeepEq(Pointer.get(doc, pointer), expected, "Pointer.get: " + label);
        else assertEq(Pointer.get(doc, pointer), expected, "Pointer.get: " + label);
    }

    // d.ts: "missing members and out-of-range indices throw"; dynajs.d.ts: "non-numeric tokens
    // against arrays throw", "'-' is refused where a real index is required".
    const GET_THROW = [
        ["a missing member throws", DOC, "/nope"],
        ["a walk through a missing member throws", DOC, "/nope/deeper"],
        ["an out-of-range index throws", DOC, "/a/b/5"],
        ["index == length is out of range for get", DOC, "/a/b/2"],
        ["a non-numeric token against an array throws", DOC, "/a/b/x"],
        ["'-' is refused where a real index is required", DOC, "/a/b/-"],
        ["a pointer deeper than 128 levels throws", deepDoc, deepPtr],
        ["a pointer over 65536 bytes throws", {}, "a".repeat(65537)],
    ];
    for (const [label, doc, pointer] of GET_THROW)
        assertThrows(() => Pointer.get(doc, pointer), "Pointer.get: " + label);
}

// d.ts: "Same walk; a missing target returns false instead of throwing";
// dynajs.d.ts: "syntax errors still throw". Rows: [label, doc, pointer, expected | "THROW"].
{
    const HAS = [
        ["an existing target -> true", DOC, "/a/b/1", true],
        ["an out-of-range index is a missing target -> false", DOC, "/a/b/2", false],
        ["a missing member -> false, no throw", DOC, "/nope/deeper", false],
        ["\"\" always exists", DOC, "", true],
        ["an empty-token key exists", { "": 1 }, "/", true],
        ["a non-numeric token is a walk error, not a missing target -> still throws", DOC, "/a/b/x", "THROW"],
        ["the over-long pointer syntax error still throws", DOC, "a".repeat(65537), "THROW"],
    ];
    for (const [label, doc, pointer, expected] of HAS) {
        if (expected === "THROW") assertThrows(() => Pointer.has(doc, pointer), "Pointer.has: " + label);
        else assertEq(Pointer.has(doc, pointer), expected, "Pointer.has: " + label);
    }
}

// d.ts: "Mutates doc in place (RFC 6902 add semantics) and returns it";
// dynajs.d.ts: "'-' appends, a missing parent throws", "Values are cloned on insert".
// Rows: [label, docTemplate, pointer, value, expectedDoc].
{
    const SET = [
        ["'-' appends to an array", DOC, "/a/b/-", "third", { a: { b: ["first", "second", "third"] } }],
        ["creates a key on an existing parent", {}, "/a", 1, { a: 1 }],
        ["over an existing member key replaces (RFC 6902 section 4.1)", { a: 1 }, "/a", 2, { a: 2 }],
        // TEST-FIX: set() pins RFC 6902 ADD semantics; section 4.1 shifts elements at or above
        // the index (replace-at-index is the section 4.3 "replace" op, which set() is not).
        ["writes an array index in place (add shifts)", { arr: [1, 2] }, "/arr/1", 9, { arr: [1, 9, 2] }],
        ["an inserted object value is a clone, not an alias", {}, "/k", { x: [1] }, { k: { x: [1] } }],
        ["escape() builds a pointer to a key containing '/'", {}, "/" + Pointer.escape("x/y"), 5, { "x/y": 5 }],
    ];
    for (const [label, template, pointer, value, expected] of SET) {
        const doc = clone(template);
        assertEq(Pointer.set(doc, pointer, value), doc, "Pointer.set: returns the caller's document itself — " + label);
        assertDeepEq(doc, expected, "Pointer.set: " + label);
    }
    // Alias check (doc pins: "the result never aliases the caller's objects").
    const src = { x: [1] }, host = {};
    Pointer.set(host, "/k", src);
    src.x.push(2);
    assertDeepEq(Pointer.get(host, "/k/x"), [1], "Pointer.set: mutating the inserted source never leaks into the doc");
    assertThrows(() => Pointer.set({}, "/a/b", 1), "Pointer.set: a missing parent throws (dynajs.d.ts)");
}

// d.ts: "Mutates doc in place and returns it"; dynajs.d.ts: "Refuses the root and a missing target".
// Rows: [label, docTemplate, pointer, expectedDoc] and [label, doc, pointer] for refusals.
{
    const REMOVE = [
        ["an object member is deleted", { a: 1, b: 2 }, "/a", { b: 2 }],
        ["an array element is spliced out (RFC 6902 section 4.3)", { arr: [1, 2, 3] }, "/arr/1", { arr: [1, 3] }],
    ];
    for (const [label, template, pointer, expected] of REMOVE) {
        const doc = clone(template);
        assertEq(Pointer.remove(doc, pointer), doc, "Pointer.remove: returns the caller's document itself — " + label);
        assertDeepEq(doc, expected, "Pointer.remove: " + label);
    }
    const REMOVE_THROW = [
        ["the root is refused (dynajs.d.ts)", { a: 1 }, ""],
        ["a missing target is refused (dynajs.d.ts)", { a: 1 }, "/nope"],
    ];
    for (const [label, doc, pointer] of REMOVE_THROW)
        assertThrows(() => Pointer.remove(doc, pointer), "Pointer.remove: " + label);
}

// d.ts: "~ -> ~0 and / -> ~1"; "Reverses escape; throws on a `~` not followed by 0 or 1".
{
    const ESCAPE = [["a/b~c", "a~1b~0c"], ["~/", "~0~1"], ["", ""], ["plain", "plain"]];
    for (const [token, expected] of ESCAPE)
        assertEq(Pointer.escape(token), expected, "Pointer.escape(" + JSON.stringify(token) + ")");
    const UNESCAPE = [["a~1b~0c", "a/b~c"], ["~0", "~"], ["~1", "/"], ["plain", "plain"]];
    for (const [token, expected] of UNESCAPE)
        assertEq(Pointer.unescape(token), expected, "Pointer.unescape(" + JSON.stringify(token) + ")");
    const UNESCAPE_THROW = [["'~2' is refused", "a~2"], ["a trailing bare '~' is refused", "z~"]];
    for (const [label, token] of UNESCAPE_THROW)
        assertThrows(() => Pointer.unescape(token), "Pointer.unescape: " + label);
}

// ------------------------------------------------------------------
// Patch (RFC 6902) — d.ts: "Runs the six RFC 6902 ops on a PRIVATE deep copy";
// dynajs.d.ts: "a failing op frees the copy and throws, leaving the input intact".
// Rows: [label, doc, ops, expected].
{
    const PATCH_OK = [
        ["dynajs.d.ts example sequence", { a: { b: [1, 2] } },
            [{ op: "add", path: "/a/b/-", value: 3 }, { op: "test", path: "/a/b/0", value: 1 }, { op: "remove", path: "/a/b/1" }],
            { a: { b: [1, 3] } }],
        ["add creates a new member", { a: 1 }, [{ op: "add", path: "/b", value: 2 }], { a: 1, b: 2 }],
        ["add over an existing member replaces (RFC 6902 section 4.1)", { a: 1 }, [{ op: "add", path: "/a", value: 2 }], { a: 2 }],
        ["add at index == length appends (RFC 6902 section 4.1)", { a: [1] }, [{ op: "add", path: "/a/1", value: 0 }], { a: [1, 0] }],
        ["replace swaps the value", { a: 1 }, [{ op: "replace", path: "/a", value: 2 }], { a: 2 }],
        ["remove deletes an object member", { a: 1, b: 2 }, [{ op: "remove", path: "/a" }], { b: 2 }],
        ["remove deletes an array element", { a: [1, 2, 3] }, [{ op: "remove", path: "/a/1" }], { a: [1, 3] }],
        ["move lands the value and empties the source (RFC 6902 section 4.4)", { a: { x: 1 } }, [{ op: "move", from: "/a", path: "/b" }], { b: { x: 1 } }],
        ["move within one array is remove-then-add with index adjustment", { a: [1, 2, 3] }, [{ op: "move", from: "/a/2", path: "/a/0" }], { a: [3, 1, 2] }],
        ["copy leaves the source in place (RFC 6902 section 4.3)", { a: { x: 1 } }, [{ op: "copy", from: "/a", path: "/b" }], { a: { x: 1 }, b: { x: 1 } }],
        ["test passes on deep-equal objects", { k: { a: [1] } }, [{ op: "test", path: "/k", value: { a: [1] } }], { k: { a: [1] } }],
        ["add at \"\" replaces the whole document (RFC 6902 section 4.1)", { a: 1 }, [{ op: "add", path: "", value: { b: 2 } }], { b: 2 }],
        ["replace at \"\" replaces the document (RFC 6902 section 4.3.2)", { a: 1 }, [{ op: "replace", path: "", value: 5 }], 5],
        ["test at \"\" works on a scalar document", "str", [{ op: "test", path: "", value: "str" }], "str"],
        ["zero ops is a pure deep copy", { k: 1 }, [], { k: 1 }],
    ];
    for (const [label, doc, ops, expected] of PATCH_OK)
        assertDeepEq(Patch.apply(doc, ops), expected, "Patch.apply: " + label);

    const PATCH_THROW = [
        ["add past the end of an array is an error (RFC 6902 section 4.1)", { a: [1] }, [{ op: "add", path: "/a/5", value: 0 }]],
        ["replace requires the target to exist (RFC 6902 section 4.3.2)", {}, [{ op: "replace", path: "/a", value: 2 }]],
        ["remove requires the target to exist", {}, [{ op: "remove", path: "/a" }]],
        ["move refuses a from that is a proper prefix of path (doc pins)", { a: { b: 1 } }, [{ op: "move", from: "/a", path: "/a/b" }]],
        ["test throws on a mismatch", { k: 1 }, [{ op: "test", path: "/k", value: 2 }]],
        ["test compares primitives via ===, so 1 !== true (doc pins)", true, [{ op: "test", path: "", value: 1 }]],
        ["only the six RFC 6902 ops exist (doc pins the op set)", {}, [{ op: "frobnicate", path: "/a" }]],
    ];
    for (const [label, doc, ops] of PATCH_THROW)
        assertThrows(() => Patch.apply(doc, ops), "Patch.apply: " + label);

    // Purity rows (reference identity, not serializable as table expectations).
    {
        const doc = { a: { b: [1, 2] } };
        const out = Patch.apply(doc, [{ op: "remove", path: "/a/b/0" }]);
        assert(out !== doc, "Patch.apply: the result is not the input object (PRIVATE deep copy)");
        assert(out.a !== doc.a, "Patch.apply: plain data is privately deep-copied");
        assertDeepEq(doc, { a: { b: [1, 2] } }, "Patch.apply: the input was never written by a successful op");
    }
    {
        const doc2 = { a: 1, list: [1, 2] };
        assertThrows(() => Patch.apply(doc2, [{ op: "remove", path: "/list/0" }, { op: "remove", path: "/nope" }]),
            "Patch.apply: a failing op throws even after earlier ops succeeded");
        assertDeepEq(doc2, { a: 1, list: [1, 2] }, "Patch.apply: partial progress never leaks into the input");
    }
    {
        // d.ts: "non-plain values (Date/RegExp/Map/TypedArray) are shared by reference".
        const stamp = new Date(0), m = new Map([[1, 2]]);
        const out = Patch.apply({ stamp, m, plain: { k: 1 } }, []);
        assert(out.stamp === stamp, "Patch.apply: Date passes by reference (doc pins the non-plain list)");
        assert(out.m === m, "Patch.apply: Map passes by reference (doc pins the non-plain list)");
        assertDeepEq(out.plain, { k: 1 }, "Patch.apply: plain branches are still copied");
    }
}

// ------------------------------------------------------------------
// Ndjson — d.ts line rules: split on \n, one trailing \r stripped, blank lines skipped.
// Rows: [text, expected].
{
    const PARSE = [
        ["dynajs.d.ts example — an empty line is skipped", '{"a":1}\n\n{"b":2}\n', [{ a: 1 }, { b: 2 }]],
        ["a whitespace-only line is skipped", "1\n \t \n2\n", [1, 2]],
        ["one trailing \\r is stripped (CRLF input)", '{"a":1}\r\n{"b":2}\r\n', [{ a: 1 }, { b: 2 }]],
        ["a missing final newline is fine", "1\n2", [1, 2]],
        ["empty text -> no values", "", []],
        ["numbers parse per line", "1\n2.5\n-3\n", [1, 2.5, -3]],
        ["scalars of every JSON kind", 'null\ntrue\n"x"\n[]\n', [null, true, "x", []]],
        ["parse(stringify(items)) is the identity (dynajs.d.ts)", Ndjson.stringify([{ i: 1 }, { i: 2 }, { i: 3 }]), [{ i: 1 }, { i: 2 }, { i: 3 }]],
    ];
    for (const [label, text, expected] of PARSE)
        assertDeepEq(Ndjson.parse(text), expected, "Ndjson.parse: " + label);

    // d.ts: malformed line -> SyntaxError naming the 1-based FILE line number (blanks count).
    const PARSE_THROW = [
        ["malformed line on file line 3 (blank line counts, result index would be 2)", '{"a":1}\n\nnope\n', "line 3"],
        // TEST-FIX: "ok" IS file line 1 and is the malformed line; the error names the 1-based
        // file line number, so the pattern is "line 1" (the row previously claimed line 2).
        ["malformed JSON on file line 1", "ok\n{\n", "line 1"],
        ["a bare \\r does not split lines — line 1 is malformed", '{"a":1}\r{"b":2}\n', "line 1"],
    ];
    for (const [label, text, pattern] of PARSE_THROW)
        assertThrows(() => Ndjson.parse(text), "Ndjson.parse: " + label, SyntaxError, pattern);

    // d.ts: "One compact JSON document per line, trailing newline included". Rows: [items, expected].
    const STRINGIFY = [
        ["objects, one per line", [{ i: 1 }, { i: 2 }], '{"i":1}\n{"i":2}\n'],
        ["scalars each take a line", [1, "two", null, true], '1\n"two"\nnull\ntrue\n'],
        ["no padding anywhere — compact per line", [{ a: [1, 2] }], '{"a":[1,2]}\n'],
    ];
    for (const [label, items, expected] of STRINGIFY)
        assertEq(Ndjson.stringify(items), expected, "Ndjson.stringify: " + label);

    // d.ts: no-JSON-form items throw a TypeError naming the 1-based index; BigInt propagates
    // JSON.stringify's own TypeError. Rows: [label, items, errType, pattern].
    const STRINGIFY_THROW = [
        ["an undefined item names index 2", [{ i: 1 }, undefined, { i: 3 }], TypeError, "2"],
        ["a function item names index 1", [function bbNope() {}, { i: 1 }], TypeError, "1"],
        ["a symbol item names index 1", [Symbol("x")], TypeError, "1"],
        ["BigInt propagates JSON.stringify's own TypeError", [1n], TypeError, "BigInt"],
    ];
    for (const [label, items, ErrType, pattern] of STRINGIFY_THROW)
        assertThrows(() => Ndjson.stringify(items), "Ndjson.stringify: " + label, ErrType, pattern);
}

print("bb_json: all tests passed (" + n + " assertions)");
