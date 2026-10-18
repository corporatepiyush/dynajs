import { Schema } from "dyna:schema";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function valid(schema, inst) { return Schema.validate(schema, inst).valid; }
function ok(schema, inst, msg) { assert(valid(schema, inst) === true, msg); }
function no(schema, inst, msg) { assert(valid(schema, inst) === false, msg); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

{
    const s = {
        properties: {
            foo: { type: "integer" },
            bar: { $ref: "#/properties/foo" },
        },
    };
    ok(s, { bar: 3 }, "ref.json#1.0: {bar:3} valid via #/properties/foo");
    no(s, { bar: true }, "ref.json#1.1: {bar:true} invalid via #/properties/foo");
}

{
    const s = {
        prefixItems: [
            { type: "integer" },
            { $ref: "#/prefixItems/0" },
        ],
    };
    ok(s, [1, 2], "ref.json#2.0: [1,2] valid via #/prefixItems/0");
    no(s, [1, "foo"], "ref.json#2.1: [1,'foo'] invalid via #/prefixItems/0");
}

{
    const s = {
        allOf: [{ type: "object", properties: { a: { type: "string" } } }],
        properties: { b: { $ref: "#/allOf/0/properties/a" } },
    };
    ok(s, { b: "x" }, "#/allOf/0/properties/a resolves (valid)");
    no(s, { b: 1 }, "#/allOf/0/properties/a resolves (invalid)");
}

throws(
    () => Schema.validate(
        { properties: { foo: { type: "integer" }, bar: { $ref: "#/foo" } } },
        { bar: 1 }),
    "ref '#/foo' must be unresolved (properties.foo lives at /properties/foo)");

{
    const s = {
        $defs: {
            "tilde~field": { type: "integer" },
            "slash/field": { type: "integer" },
            "percent%field": { type: "integer" },
        },
        properties: {
            tilde: { $ref: "#/$defs/tilde~0field" },
            slash: { $ref: "#/$defs/slash~1field" },
            percent: { $ref: "#/$defs/percent%25field" },
        },
    };
    ok(s, { slash: 123 }, "ref.json#3.3: %25 alongside ~1 (slash valid)");
    no(s, { slash: "aoeu" }, "ref.json#3.0: %25 alongside ~1 (slash invalid)");
    ok(s, { tilde: 123 }, "ref.json#3.4: %25 alongside ~0 (tilde valid)");
    no(s, { tilde: "aoeu" }, "ref.json#3.1: %25 alongside ~0 (tilde invalid)");
    ok(s, { percent: 123 }, "ref.json#3.5: %25 decodes to '%' (valid)");
    no(s, { percent: "aoeu" }, "ref.json#3.2: %25 decodes to '%' (invalid)");
}

{
    const s = {
        properties: { 'foo"bar': { $ref: "#/$defs/foo%22bar" } },
        $defs: { 'foo"bar': { type: "number" } },
    };
    ok(s, { 'foo"bar': 1 }, "ref.json#12.0: %22 decodes to '\"' (number valid)");
    no(s, { 'foo"bar': "1" }, "ref.json#12.1: %22 decodes to '\"' (string invalid)");
}

{
    const s = {
        $defs: { "café": { type: "integer" } },
        properties: { k: { $ref: "#/$defs/caf%C3%A9" } },
    };
    ok(s, { k: 1 }, "RFC 3986 §2.5: %C3%A9 decodes to UTF-8 'é' (valid)");
    no(s, { k: "x" }, "RFC 3986 §2.5: %C3%A9 decodes to UTF-8 'é' (invalid)");
}

if (fails) {
    print("test_schema_ref_paths: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_schema_ref_paths failed");
}
print("test_schema_ref_paths: " + n + " assertions, 0 failures");
