// covers: dyna:schema.Schema.validate
// F-schema-1: validate used to build the escaped JSON-pointer path string ("/a~1b/0") on every descent into a
// property or item, even for documents with no error (~5% of validating a valid document). The descent now keeps a
// stack of (token, length) slices into the caller's own key and index buffers, and the escaped path is built only
// when an error is recorded. Measured: a 20-order valid document, 10000 validations, CPU best of 15: 64.5-65.1 ms
// before, 60.4-60.7 ms after (load average 50-74 on a shared Mac).
// Oracle: an independent JSON pointer built in JS from the location each error is planted at (RFC 6901: "~" -> "~0",
// "/" -> "~1"), over keys with "~", "/", both, empty and non-ASCII, nesting deeper than the 16-slot initial stack,
// array indices, and errors in sibling subtrees after a deep descent (the stack must be popped back exactly).
import { suite } from "../lib/kit.js";
import { Schema } from "dyna:schema";

const t = suite("test_schema_schema_validate_error_path_lazy");
const ptr = (segs) => segs.map((s) => "/" + String(s).replace(/~/g, "~0").replace(/\//g, "~1")).join("");
const paths = (r) => r.errors.map((e) => e.path + " " + e.keyword).sort();

t.test("valid documents have no errors at any depth", ({ eq }) => {
    const s = Schema.compile({ type: "object", additionalProperties: { $ref: "#" } });
    let d = {};
    for (let i = 0; i < 60; i++) d = { ["k/" + i]: d };
    eq(s.validate(d).valid, true, "60 levels");
});

t.test("escaped keys, empty and non-ASCII keys", ({ eq, deep }) => {
    const keys = ["a/b", "t~1", "", "é/~", "~~//", "plain", "x".repeat(300) + "/"];
    const doc = {};
    for (const k of keys) doc[k] = "not an int";
    doc.ok = 3;
    const r = Schema.validate({ type: "object", additionalProperties: { type: "integer" } }, doc);
    eq(r.valid, false, "invalid");
    deep(paths(r), keys.map((k) => ptr([k]) + " type").sort(), "paths");
});

t.test("deep nesting past the initial stack, then a sibling", ({ eq, deep }) => {
    const segs = [];
    let leaf = { bad: "s" };
    const want = [];
    for (let i = 39; i >= 0; i--) {
        const k = i % 3 === 0 ? "n/" + i : i % 3 === 1 ? "m~" + i : "p" + i;
        leaf = i % 4 === 2 ? [0, leaf] : { [k]: leaf };
        segs.unshift(i % 4 === 2 ? 1 : k);
    }
    want.push(ptr(["deep", ...segs, "bad"]) + " type");
    const doc = { deep: leaf, after: "s", list: [1, "s", 2, { z: "s" }] };
    want.push(ptr(["after"]) + " type", ptr(["list", 1]) + " type", ptr(["list", 3, "z"]) + " type");
    const schema = { $defs: { n: { type: ["integer", "object", "array"], additionalProperties: { $ref: "#/$defs/n" }, items: { $ref: "#/$defs/n" } } },
        type: "object", additionalProperties: { $ref: "#/$defs/n" } };
    const r = Schema.validate(schema, doc);
    eq(r.valid, false, "invalid");
    deep(paths(r), want.sort(), "every error at its own pointer, nothing stale");
});

t.test("properties, items, prefixItems, patternProperties paths", ({ eq, deep }) => {
    const schema = {
        type: "object",
        properties: { "a/b": { type: "array", prefixItems: [{ type: "string" }], items: { type: "integer" } } },
        patternProperties: { "^p~": { type: "boolean" } },
    };
    const r = Schema.validate(schema, { "a/b": [1, 2, "x", 4], "p~q": 1 });
    deep(paths(r), [ptr(["a/b", 0]) + " type", ptr(["a/b", 2]) + " type", ptr(["p~q"]) + " type"].sort(), "paths");
});

await t.run();
