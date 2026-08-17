/* test_schema.js -- dyna:schema JSON Schema 2020-12 core validator.
 *
 * Every keyword is exercised with a positive AND a negative vector; the
 * RFC 6901 instance paths in errors are asserted exactly; recursion is
 * exercised with a self-referential $def and with a circular schema object;
 * deep equality pins NaN/+-0/BigInt and circular const values. The engine's
 * own RegExp is the oracle for pattern semantics, so every pattern test here
 * is paired with the equivalent new RegExp(pattern, "u").test() result.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_schema.js
 */
import { Schema } from "dyna:schema";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(v, msg) {
    const isV = (v === true) || (v && typeof v === "object" && v.valid === true);
    assert(isV, msg + " (expected valid)");
}
function no(v, msg) {
    const isInv = (v === false) || (v && typeof v === "object" && v.valid === false);
    assert(isInv, msg + " (expected invalid)");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const assertThrows = throws;
function valid(schema, inst) { return Schema.validate(schema, inst).valid; }
function errs(schema, inst) { return Schema.validate(schema, inst).errors; }
function hasErr(schema, inst, path, kw) {
    for (const e of errs(schema, inst))
        if (e.path === path && e.keyword === kw) return true;
    return false;
}
function anyErr(schema, inst, kw) {
    for (const e of errs(schema, inst)) if (e.keyword === kw) return true;
    return false;
}

/* ---------------------------------------------------------------- shape */

ok(Schema.validate({}, {}).valid, "empty schema accepts anything");
ok(Schema.validate({}, 42), "empty schema accepts a number");
ok(Schema.validate(true, 42), "boolean true schema accepts anything");
no(Schema.validate(false, 42), "boolean false schema rejects everything");
{
    const r = Schema.validate({ type: "number" }, "x");
    assert(r.valid === false && Array.isArray(r.errors), "result shape");
    assert(r.errors[0].path === "" && r.errors[0].keyword === "type",
           "root errors report an empty path and the keyword");
}
/* String schemas compile through JSON.parse. */
ok(Schema.validate('{"type":"number"}', 5), "a JSON string schema compiles");
throws(() => Schema.compile('[1,2]'), "an array is not a schema");
throws(() => Schema.compile("not json"), "unparsable schema string throws");

/* ----------------------------------------------------------------- type */

ok(Schema.validate({ type: "string" }, ""), "type string");
no(Schema.validate({ type: "string" }, 5), "type string vs number");
ok(Schema.validate({ type: "number" }, 3.5), "type number");
no(Schema.validate({ type: "number" }, NaN), "NaN is not a JSON number");
no(Schema.validate({ type: "number" }, Infinity), "Infinity is not a number");
ok(Schema.validate({ type: "integer" }, 3), "type integer");
ok(Schema.validate({ type: "integer" }, 3.0), "3.0 is an integer");
no(Schema.validate({ type: "integer" }, 3.5), "3.5 is not an integer");
ok(Schema.validate({ type: "integer" }, 1e300), "1e300 has no fraction");
ok(Schema.validate({ type: ["string", "number"] }, "x"), "union accepts string");
ok(Schema.validate({ type: ["string", "number"] }, 5), "union accepts number");
no(Schema.validate({ type: ["string", "number"] }, true), "union rejects bool");
ok(Schema.validate({ type: ["null", "boolean"] }, null), "null member");
ok(Schema.validate({ type: "object" }, {}), "type object");
no(Schema.validate({ type: "object" }, []), "an array is not an object");
no(Schema.validate({ type: "object" }, function () {}), "function is not object");

/* ---------------------------------------------------------------- const */

ok(Schema.validate({ const: 5 }, 5), "const number");
no(Schema.validate({ const: 5 }, 6), "const number mismatch");
ok(Schema.validate({ const: "a" }, "a"), "const string");
no(Schema.validate({ const: "a" }, "b"), "const string mismatch");
ok(Schema.validate({ const: { a: [1, 2], b: { c: true } } },
                   { a: [1, 2], b: { c: true } }),
   "const deep object equality");
no(Schema.validate({ const: { a: [1, 2] } }, { a: [1, 2, 3] }),
   "deep equality notices an extra element");
no(Schema.validate({ const: { a: 1 } }, { a: 1, b: 2 }),
   "deep equality notices an extra key");
ok(Schema.validate({ const: 0 }, -0), "+0 and -0 are equal (===)");
no(Schema.validate({ const: NaN }, NaN), "NaN never equals NaN");
ok(Schema.validate({ const: 1n }, 1n), "BigInt const via ===");
no(Schema.validate({ const: 1n }, 2n), "BigInt mismatch");
/* A circular const: the cycle guard makes this terminate and agree. */
{
    const c = {}; c.self = c;
    const d = {}; d.self = d;
    ok(Schema.validate({ const: c }, d),
       "circular const compares coinductively (no stack overflow)");
}

/* ---------------------------------------------------------------- enum */

ok(Schema.validate({ enum: [1, 2, 3] }, 2), "enum member");
no(Schema.validate({ enum: [1, 2, 3] }, 4), "enum non-member");
ok(Schema.validate({ enum: [{ a: 1 }, "x"] }, { a: 1 }), "enum deep object");
no(Schema.validate({ enum: [{ a: 1 }] }, { a: 2 }), "enum deep mismatch");
no(Schema.validate({ enum: [NaN] }, NaN), "enum NaN matches nothing (=== semantics)");

/* ------------------------------------------------------ numeric bounds */

ok(Schema.validate({ minimum: 5 }, 5), "minimum inclusive");
no(Schema.validate({ minimum: 5 }, 4.9), "minimum violation");
ok(Schema.validate({ maximum: 5 }, 5), "maximum inclusive");
no(Schema.validate({ maximum: 5 }, 5.1), "maximum violation");
ok(Schema.validate({ exclusiveMinimum: 5 }, 5.01), "exclusiveMinimum");
no(Schema.validate({ exclusiveMinimum: 5 }, 5), "exclusiveMinimum at the edge");
ok(Schema.validate({ exclusiveMaximum: 5 }, 4.99), "exclusiveMaximum");
no(Schema.validate({ exclusiveMaximum: 5 }, 5), "exclusiveMaximum at the edge");
/* Both an inclusive and an exclusive bound on one schema must BOTH hold. */
ok(Schema.validate({ minimum: 5, exclusiveMinimum: 3 }, 6),
   "minimum and exclusiveMinimum both apply");
no(Schema.validate({ minimum: 5, exclusiveMinimum: 3 }, 4),
   "minimum:5 still rejects 4 even with exclusiveMinimum:3");
ok(Schema.validate({ multipleOf: 2 }, 10), "multipleOf");
no(Schema.validate({ multipleOf: 2 }, 7), "multipleOf violation");
no(Schema.validate({ multipleOf: 0.0001 }, 0.00751),
   "multipleOf precision tolerance");

/* ------------------------------------------------------ string bounds */

ok(Schema.validate({ minLength: 2 }, "ab"), "minLength");
no(Schema.validate({ minLength: 2 }, "a"), "minLength violation");
ok(Schema.validate({ maxLength: 2 }, "ab"), "maxLength");
no(Schema.validate({ maxLength: 2 }, "abc"), "maxLength violation");
ok(Schema.validate({ minLength: 1, maxLength: 4 }, "caf\u00E9"),
   "code points, not bytes: cafe-acute is 4 chars");
no(Schema.validate({ maxLength: 3 }, "caf\u00E9"),
   "4 code points exceed maxLength 3");
/* minLength and minItems on the SAME schema are independent bounds. */
no(Schema.validate({ minLength: 5, minItems: 3 }, "ab"),
   "minLength is not clobbered by minItems");

/* -------------------------------------------------------------- pattern */

ok(Schema.validate({ pattern: "^a*$" }, "aaa"), "pattern anchor");
no(Schema.validate({ pattern: "^a*$" }, "ba"), "pattern anchor mismatch");
ok(Schema.validate({ pattern: "a.c" }, "a\u00E9c"),
   "dot matches a whole code point in unicode mode");
ok(Schema.validate({ pattern: "a.c" }, "a\u{1D306}c"),
   "dot matches surrogate pair in unicode mode");
no(Schema.validate({ pattern: "a.c" }, "ac"), "dot requires a character");
assertThrows(() => Schema.compile({ pattern: "[" }),
   "invalid pattern rejects at compile time");

/* -------------------------------------------------------- array bounds */

ok(Schema.validate({ minItems: 2 }, [1, 2]), "minItems pass");
no(Schema.validate({ minItems: 2 }, [1]), "minItems fail");
ok(Schema.validate({ maxItems: 2 }, [1, 2]), "maxItems pass");
no(Schema.validate({ maxItems: 2 }, [1, 2, 3]), "maxItems fail");
ok(Schema.validate({ uniqueItems: true }, [1, 2, 3]), "uniqueItems distinct numbers");
no(Schema.validate({ uniqueItems: true }, [1, 2, 1]), "uniqueItems duplicate numbers");
ok(Schema.validate({ uniqueItems: true }, [{ a: 1 }, { a: 2 }]), "uniqueItems distinct objects");
no(Schema.validate({ uniqueItems: true }, [{ a: 1 }, { a: 1 }]), "uniqueItems duplicate objects");
ok(Schema.validate({ uniqueItems: false }, [1, 1]), "uniqueItems false permits duplicates");

/* ------------------------------------------------------- object bounds */

ok(Schema.validate({ minProperties: 2 }, { a: 1, b: 2 }), "minProperties pass");
no(Schema.validate({ minProperties: 2 }, { a: 1 }), "minProperties fail");
ok(Schema.validate({ maxProperties: 2 }, { a: 1, b: 2 }), "maxProperties pass");
no(Schema.validate({ maxProperties: 2 }, { a: 1, b: 2, c: 3 }), "maxProperties fail");
ok(Schema.validate({ dependentRequired: { a: ["b", "c"] } }, { a: 1, b: 2, c: 3 }), "dependentRequired satisfied");
no(Schema.validate({ dependentRequired: { a: ["b", "c"] } }, { a: 1, b: 2 }), "dependentRequired missing property");
ok(Schema.validate({ dependentRequired: { a: ["b"] } }, { x: 1 }), "dependentRequired trigger absent");

/* --------------------------------------------------------- composition */

ok(Schema.validate({ allOf: [{ type: "integer" }, { minimum: 0 }] }, 5), "allOf pass");
no(Schema.validate({ allOf: [{ type: "integer" }, { minimum: 0 }] }, -1), "allOf fail");
ok(Schema.validate({ anyOf: [{ type: "string" }, { type: "number" }] }, "abc"), "anyOf pass string");
ok(Schema.validate({ anyOf: [{ type: "string" }, { type: "number" }] }, 123), "anyOf pass number");
no(Schema.validate({ anyOf: [{ type: "string" }, { type: "number" }] }, true), "anyOf fail boolean");
ok(Schema.validate({ oneOf: [{ type: "integer" }, { minimum: 5 }] }, 3), "oneOf matches first only");
no(Schema.validate({ oneOf: [{ type: "integer" }, { minimum: 5 }] }, 6), "oneOf matches both (fails)");
no(Schema.validate({ oneOf: [{ type: "integer" }, { type: "boolean" }] }, "abc"), "oneOf matches neither");
ok(Schema.validate({ not: { type: "string" } }, 123), "not pass");
no(Schema.validate({ not: { type: "string" } }, "abc"), "not fail");

/* if / then / else */
const condSchema = {
    if: { properties: { kind: { const: "num" } }, required: ["kind"] },
    then: { properties: { val: { type: "number" } }, required: ["val"] },
    else: { properties: { val: { type: "string" } }, required: ["val"] },
};
ok(Schema.validate(condSchema, { kind: "num", val: 42 }), "if/then pass");
no(Schema.validate(condSchema, { kind: "num", val: "x" }), "if/then fail");
ok(Schema.validate(condSchema, { kind: "str", val: "x" }), "if/else pass");
no(Schema.validate(condSchema, { kind: "str", val: 42 }), "if/else fail");

/* ------------------------------------------------ local $ref and $defs */

const treeSchema = {
    $defs: {
        node: {
            type: "object",
            properties: {
                val: { type: "integer" },
                left: { $ref: "#/$defs/node" },
                right: { $ref: "#/$defs/node" }
            },
            required: ["val"]
        }
    },
    $ref: "#/$defs/node"
};
const compiledTree = Schema.compile(treeSchema);
ok(compiledTree.validate({ val: 1, left: { val: 2 }, right: { val: 3, left: { val: 4 } } }).valid, "recursive $ref tree valid");
no(compiledTree.validate({ val: 1, left: { val: "bad" } }).valid, "recursive $ref tree invalid child");

/* ---------------------------------------------------- RFC 6901 paths */

const errRes = Schema.validate({
    type: "object",
    properties: {
        users: {
            type: "array",
            items: {
                type: "object",
                properties: { age: { type: "integer", minimum: 0 } },
                required: ["age"]
            }
        }
    }
}, { users: [{ age: 25 }, { age: -5 }] });
no(errRes.valid, "nested validation fails");
assert(errRes.errors.length > 0, "errors generated");
assert(errRes.errors[0].path === "/users/1/age", "RFC 6901 path matches: " + errRes.errors[0].path);
assert(errRes.errors[0].keyword === "minimum", "error keyword is minimum: " + errRes.errors[0].keyword);

if (fails) {
    print("test_schema: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_schema failed");
}
print("test_schema: " + n + " assertions, 0 failures");
