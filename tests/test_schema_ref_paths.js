/* test_schema_ref_paths.js -- dyna:schema $ref evaluation paths + fragment
 * percent-decoding (audit M24-02).
 *
 * Two behaviors pinned, both previously divergent from 2020-12:
 *  1. A same-document $ref is an RFC 6901 pointer into the schema DOCUMENT
 *     (2020-12 §8.2.3.2): a subschema under an applicator keyword lives at
 *     its keyword-scoped location, so "#/properties/foo" must reach
 *     properties.foo and "#/prefixItems/0" must reach prefixItems[0]. The
 *     old compiler indexed applicator subschemas at the bare /<name> or
 *     /<index>, leaving exactly those refs unresolvable.
 *  2. The URI fragment of a $ref is percent-decoded BEFORE it is read as a
 *     JSON pointer (2020-12 §8.2.3.2 with RFC 3986 §2.4: %XX -> octet, then
 *     the octet sequence is UTF-8), so "#/$defs/percent%25field" addresses
 *     the $defs key "percent%field".
 *
 * Expected values are sourced from the vendored official-suite corpus
 * (tests/corpus/jsonschema/draft2020-12/ref.json groups 1, 2, 3, 12 -- each
 * group named below) and from the spec sections above, NOT from this
 * engine's output.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_schema_ref_paths.js
 */
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

/* ---- 1. $ref evaluation paths through applicator keywords ------------- */

/* corpus ref.json group 1 "relative pointer ref to object" (both tests) */
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

/* corpus ref.json group 2 "relative pointer ref to array" (both tests) */
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

/* spec-derived (2020-12 §8.2.3.2): pointer segments chain through
 * composition keywords the same way -- /allOf/0/properties/a is the
 * evaluation path of that nested subschema. */
{
    const s = {
        allOf: [{ type: "object", properties: { a: { type: "string" } } }],
        properties: { b: { $ref: "#/allOf/0/properties/a" } },
    };
    ok(s, { b: "x" }, "#/allOf/0/properties/a resolves (valid)");
    no(s, { b: 1 }, "#/allOf/0/properties/a resolves (invalid)");
}

/* spec-derived (2020-12 §8.2.3.2): the pointer addresses the document, so
 * the pre-fix misregistered short path "#/foo" is NOT a schema location and
 * must not resolve. */
throws(
    () => Schema.validate(
        { properties: { foo: { type: "integer" }, bar: { $ref: "#/foo" } } },
        { bar: 1 }),
    "ref '#/foo' must be unresolved (properties.foo lives at /properties/foo)");

/* ---- 2. fragment percent-decoding before pointer resolution ------------ */

/* corpus ref.json group 3 "escaped pointer ref": %25 -> '%', plus the
 * ~0/~1 escapes that must keep working alongside it. */
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

/* corpus ref.json group 12 "refs with quote": %22 -> '"' */
{
    const s = {
        properties: { 'foo"bar': { $ref: "#/$defs/foo%22bar" } },
        $defs: { 'foo"bar': { type: "number" } },
    };
    ok(s, { 'foo"bar': 1 }, "ref.json#12.0: %22 decodes to '\"' (number valid)");
    no(s, { 'foo"bar': "1" }, "ref.json#12.1: %22 decodes to '\"' (string invalid)");
}

/* spec-derived (RFC 3986 §2.5): a percent-encoded UTF-8 sequence decodes
 * octet-wise back to the original key bytes. */
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
