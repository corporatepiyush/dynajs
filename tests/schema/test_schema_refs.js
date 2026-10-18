// dyna:schema referencing and option surface, case by case (the JSON Schema Test Suite run in
// test_schema_conformance.js is the bulk oracle; these pin the API behaviour and the refusals by name):
// $id base-URI changes with RFC 3986 dot-segment resolution, $anchor, $dynamicRef dynamic scope, the resources
// and baseUri options, the embedded 2020-12 meta-schemas, the three format modes, $vocabulary from a custom
// meta-schema, refs into unknown keywords, the legacy "dependencies" keyword, and every new refusal.
import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const valid = (schema, data, opts) => Schema.compile(schema, opts).validate(data).valid;
const throwsT = (f, re, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof TypeError && re.test(e.message), m + " -- " + e.name + ": " + e.message); } };

{
    const s = { $id: "https://ex.test/a/b/root.json", $defs: { x: { $id: "../c/x.json", type: "integer" } }, $ref: "https://ex.test/a/c/x.json" };
    ok(valid(s, 3) && !valid(s, "3"), "embedded $id resolves ../ against the parent base (RFC 3986 5.2.4)");
    const rel = { $id: "https://ex.test/root.json", $defs: { x: { $id: "nested/x.json", type: "string" } }, properties: { p: { $ref: "nested/x.json" } } };
    ok(valid(rel, { p: "s" }) && !valid(rel, { p: 1 }), "a relative $ref resolves against the enclosing $id");
    const urn = { $id: "urn:uuid:deadbeef-1234-ffff-ffff-4321feebdaed", $defs: { b: { type: "boolean" } }, $ref: "urn:uuid:deadbeef-1234-ffff-ffff-4321feebdaed#/$defs/b" };
    ok(valid(urn, true) && !valid(urn, 0), "a URN base with a JSON-pointer fragment");
}
{
    const s = { $defs: { pos: { $anchor: "positive", minimum: 1 } }, items: { $ref: "#positive" } };
    ok(valid(s, [1, 2]) && !valid(s, [1, 0]), "$anchor plain-name fragments");
    const inner = { $id: "https://ex.test/outer", $defs: { r: { $id: "inner", $anchor: "a", type: "null" } }, $ref: "inner#a" };
    ok(valid(inner, null) && !valid(inner, 1), "an anchor is scoped to its own resource");
}
{
    // The canonical extension pattern: a generic list whose item schema is a dynamic anchor, specialised by a
    // second resource that redefines the anchor. The outermost resource in the dynamic scope wins.
    const generic = { $id: "https://ex.test/list", $defs: { item: { $dynamicAnchor: "item" } }, type: "array", items: { $dynamicRef: "#item" } };
    const ofStrings = { $id: "https://ex.test/strings", $ref: "https://ex.test/list", $defs: { item: { $dynamicAnchor: "item", type: "string" } } };
    const opts = { resources: { "https://ex.test/list": generic } };
    ok(valid(ofStrings, ["a", "b"], opts) && !valid(ofStrings, ["a", 1], opts), "$dynamicRef binds to the outermost $dynamicAnchor in scope");
    ok(valid(generic, ["a", 1]), "without the extension the generic list accepts anything");
    const notBookended = { $id: "https://ex.test/nb", $defs: { t: { $anchor: "item", type: "number" } }, items: { $dynamicRef: "#item" } };
    ok(valid(notBookended, [1]) && !valid(notBookended, ["x"]), "a fragment that is a plain $anchor behaves like $ref");
}
{
    const resources = { "https://schemas.test/id.json": { type: "string", pattern: "^[a-z]+-[0-9]+$" }, "https://schemas.test/defs.json": { $defs: { n: { type: "number" } } } };
    const s = { properties: { id: { $ref: "https://schemas.test/id.json" }, n: { $ref: "https://schemas.test/defs.json#/$defs/n" } } };
    ok(valid(s, { id: "ab-1", n: 2 }, { resources }) && !valid(s, { id: "AB", n: 2 }, { resources }) && !valid(s, { id: "ab-1", n: "2" }, { resources }),
        "resources serve remote documents and pointers into them");
    ok(valid({ $ref: "id.json" }, "ab-1", { resources, baseUri: "https://schemas.test/root.json" }), "baseUri anchors relative refs in a schema with no $id");
    throwsT(() => Schema.compile({ $ref: "https://schemas.test/missing.json" }, { resources }), /no schema resource "https:\/\/schemas.test\/missing.json".*resources/, "an absent remote is refused by name");
}
{
    const meta = { $ref: "https://json-schema.org/draft/2020-12/schema" };
    ok(valid(meta, { type: "string", minLength: 2 }), "a schema validates against the embedded 2020-12 meta-schema");
    ok(!valid(meta, { type: 1 }) && !valid(meta, { minLength: -1 }) && !valid(meta, { properties: { a: 3 } }), "invalid schemas fail the embedded meta-schema");
    ok(valid(meta, { $defs: { a: { $dynamicRef: "#meta" } } }), "the meta-schema's own $dynamicRef '#meta' chain resolves");
}
{
    const email = { format: "email" }, date = { format: "date" };
    ok(valid(email, "not an email") && valid(date, "2026-02-30"), "default: built-in formats are annotations (2020-12)");
    ok(!valid(email, "not an email", { formats: "assertion" }) && valid(email, "a@b.test", { formats: "assertion" }), "assertion mode checks email");
    ok(!valid(date, "2026-02-30", { formats: "assertion" }) && valid(date, "2024-02-29", { formats: "assertion" }), "assertion mode checks calendar dates");
    ok(!valid({ format: "time" }, "22:59:60Z", { formats: "assertion" }) && valid({ format: "time" }, "23:59:60Z", { formats: "assertion" }), "a leap second only at 23:59 UTC");
    ok(!valid({ format: "regex" }, "(", { formats: "assertion" }) && valid({ format: "unknown-fmt" }, "x", { formats: "assertion" }), "regex is compiled; unknown formats are ignored");
    ok(valid(email, 42, { formats: "assertion" }), "formats only apply to strings");
    ok(!valid({ format: "idn-hostname" }, "ex-¡.test", { formats: "assertion" }) && valid({ format: "idn-hostname" }, "bücher.example", { formats: "assertion" }), "idn-hostname through IDNA");
    Schema.registerFormat("even-length-refs", (v) => v.length % 2 === 0);
    ok(!valid({ format: "even-length-refs" }, "abc") && valid({ format: "even-length-refs" }, "abc", { formats: "annotation" }), "annotation mode switches registered formats off too");
}
{
    const noValidation = { $schema: "https://ex.test/meta-nv", type: "integer", properties: { a: { minimum: 10 } } };
    const resources = { "https://ex.test/meta-nv": { $schema: "https://json-schema.org/draft/2020-12/schema", $vocabulary: { "https://json-schema.org/draft/2020-12/vocab/core": true, "https://json-schema.org/draft/2020-12/vocab/applicator": true } } };
    ok(valid(noValidation, { a: 1 }, { resources }), "a meta-schema without the validation vocabulary switches validation keywords off");
    const fa = { "https://ex.test/meta-fa": { $vocabulary: { "https://json-schema.org/draft/2020-12/vocab/core": true, "https://json-schema.org/draft/2020-12/vocab/validation": true, "https://json-schema.org/draft/2020-12/vocab/format-assertion": true } } };
    ok(!valid({ $schema: "https://ex.test/meta-fa", format: "ipv4" }, "999.1.1.1", { resources: fa }), "the format-assertion vocabulary turns assertion on");
    const req = { "https://ex.test/meta-x": { $vocabulary: { "https://ex.test/vocab/unknown": true } } };
    throwsT(() => Schema.compile({ $schema: "https://ex.test/meta-x" }, { resources: req }), /requires vocabulary "https:\/\/ex.test\/vocab\/unknown"/, "a required unknown vocabulary is refused");
    const optional = { "https://ex.test/meta-o": { $vocabulary: { "https://ex.test/vocab/unknown": false, "https://json-schema.org/draft/2020-12/vocab/validation": true } } };
    ok(!valid({ $schema: "https://ex.test/meta-o", type: "string" }, 1, { resources: optional }), "an optional unknown vocabulary is ignored");
}
{
    const s = { $defs: { other: { stuff: { type: "integer" } } }, $ref: "#/$defs/other/stuff" };
    ok(valid(s, 1) && !valid(s, "1"), "a $ref into an unknown keyword compiles that location on demand");
    const deps = { dependencies: { a: ["b"], c: { required: ["d"] } } };
    ok(!valid(deps, { a: 1 }) && valid(deps, { a: 1, b: 1 }) && !valid(deps, { c: 1 }) && valid(deps, { c: 1, d: 0 }), "legacy dependencies splits into dependentRequired / dependentSchemas");
    const thenId = { allOf: [{ $ref: "https://ex.test/then" }], then: { $id: "https://ex.test/then", type: "integer" } };
    ok(valid(thenId, 1) && !valid(thenId, "a"), "a then without if still declares its $id resource");
}
{
    throwsT(() => Schema.compile({ $id: "https://ex.test/a#frag" }), /must not carry a fragment/, "$id with a fragment is refused");
    throwsT(() => Schema.compile({ $defs: { a: { $id: "https://ex.test/x" }, b: { $id: "https://ex.test/x" } } }), /duplicate "\$id"/, "a duplicate $id is refused");
    throwsT(() => Schema.compile({ $anchor: "1bad" }), /not a plain name/, "an anchor that is not a plain name is refused");
    throwsT(() => Schema.compile({ $defs: { a: { $anchor: "x" }, b: { $anchor: "x" } } }), /duplicate anchor "x"/, "duplicate anchors in one resource are refused");
    throwsT(() => Schema.compile({}, { resolve: true }), /unknown option "resolve"/, "unknown options are refused");
    throwsT(() => Schema.compile({}, { formats: "strict" }), /formats must be/, "an unknown formats mode is refused");
    throwsT(() => Schema.compile({}, { resources: [] }), /resources must be an object/, "resources must be a map");
    throwsT(() => Schema.compile({}, { baseUri: "relative/path" }), /baseUri must be an absolute URI/, "a relative baseUri is refused");
    throwsT(() => Schema.compile({ $ref: "#nowhere" }), /unresolved \$ref "#nowhere"/, "an unknown anchor is refused");
    throwsT(() => Schema.compile({ $dynamicRef: "#/$defs/missing" }), /unresolved \$dynamicRef/, "an unresolved $dynamicRef is refused");
}
{
    const big = { $id: "https://ex.test/big", $defs: {} };
    for (let i = 0; i < 300; i++) big.$defs["d" + i] = { $id: "d" + i, $anchor: "a" + i, type: i % 2 ? "string" : "number" };
    const c = Schema.compile({ ...big, items: { $ref: "d299#a299" } });
    ok(c.validate(["s"]).valid && !c.validate([1]).valid, "300 embedded resources resolve");
}
print("test_schema_refs: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
