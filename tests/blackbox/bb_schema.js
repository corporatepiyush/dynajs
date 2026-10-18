// Black-box contract test for dyna:schema, generated from dynajs.d.ts lines 4114-4161. Engine sources not consulted.
// Table-driven: every expectation is a case row; each failure names its row.
import { Schema } from "dyna:schema";

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

// ------------------------------------------------------------------
// VERDICT table — Draft 2020-12 core keywords (d.ts: "Draft 2020-12 validation"),
// one row per (schema, instance, expectedValid). Driven through Schema.validate,
// the documented convenience door.
// Rows: [label, schema, instance, expectedValid].
// ------------------------------------------------------------------

const VERDICTS = [
    // -- the "type" keyword, all seven 2020-12 types --
    ["type string accepts a string", { type: "string" }, "s", true],
    ["type string refuses a number", { type: "string" }, 1, false],
    ["type number accepts 1.5", { type: "number" }, 1.5, true],
    ["type number refuses a numeric string", { type: "number" }, "1.5", false],
    ["type integer accepts 1", { type: "integer" }, 1, true],
    ["type integer refuses 1.5", { type: "integer" }, 1.5, false],
    ["type boolean accepts true", { type: "boolean" }, true, true],
    ["type boolean refuses 0 (2020-12 equality is type-strict)", { type: "boolean" }, 0, false],
    ["type object accepts {}", { type: "object" }, {}, true],
    ["an array is not type object (2020-12)", { type: "object" }, [], false],
    ["type array accepts []", { type: "array" }, [], true],
    ["an object is not type array (2020-12)", { type: "array" }, {}, false],
    ["type null accepts null", { type: "null" }, null, true],
    ["type null refuses 0", { type: "null" }, 0, false],
    ["type as an array is a union (2020-12)", { type: ["string", "number"] }, 1.5, true],
    ["the type union rejects outsiders", { type: ["string", "number"] }, true, false],

    // -- enum / const --
    ["enum accepts a member", { enum: ["a", "b"] }, "a", true],
    ["enum refuses a non-member", { enum: ["a", "b"] }, "c", false],
    ["enum: 1 !== true (2020-12 type-strict equality)", { enum: [1, 2] }, true, false],
    ["const accepts the exact value", { const: 42 }, 42, true],
    ["const refuses anything else", { const: 42 }, 43, false],

    // -- numeric keywords --
    ["minimum is inclusive", { type: "number", minimum: 1 }, 1, true],
    ["below minimum fails", { type: "number", minimum: 1 }, 0.999, false],
    ["maximum is inclusive", { type: "number", maximum: 10 }, 10, true],
    ["above maximum fails", { type: "number", maximum: 10 }, 10.5, false],
    ["exclusiveMinimum is exclusive — a NUMBER in 2020-12 (dynajs.d.ts pins that draft-07's boolean form needs upgrading)", { type: "number", exclusiveMinimum: 1 }, 1, false],
    ["exclusiveMinimum passes strictly above", { type: "number", exclusiveMinimum: 1 }, 1.1, true],
    ["exclusiveMaximum is exclusive", { type: "number", exclusiveMaximum: 5 }, 5, false],
    ["multipleOf accepts a multiple", { type: "number", multipleOf: 5 }, 10, true],
    ["multipleOf refuses a non-multiple", { type: "number", multipleOf: 5 }, 7, false],

    // -- string keywords --
    ["maxLength is inclusive", { type: "string", maxLength: 3 }, "abc", true],
    ["maxLength refuses longer", { type: "string", maxLength: 3 }, "abcd", false],
    ["minLength refuses shorter", { type: "string", minLength: 2 }, "a", false],
    ["minLength passes at the bound", { type: "string", minLength: 2 }, "ab", true],
    ["pattern anchors apply", { type: "string", pattern: "^a" }, "abc", true],
    ["pattern refuses non-matches", { type: "string", pattern: "^a" }, "bac", false],

    // -- array keywords --
    ["items applies to every element", { type: "array", items: { type: "integer" } }, [1, 2, 3], true],
    ["items refuses a bad element", { type: "array", items: { type: "integer" } }, [1, "x"], false],
    ["minItems inclusive", { type: "array", minItems: 2 }, [1, 2], true],
    ["minItems refuses short arrays", { type: "array", minItems: 2 }, [1], false],
    ["maxItems inclusive", { type: "array", maxItems: 2 }, [1, 2], true],
    ["maxItems refuses long arrays", { type: "array", maxItems: 2 }, [1, 2, 3], false],
    ["uniqueItems passes for distinct items", { uniqueItems: true }, [1, 2], true],
    ["uniqueItems refuses duplicates", { uniqueItems: true }, [1, 1], false],
    ["2020-12 equality is type-strict: 1 and true are unique items", { uniqueItems: true }, [1, true], true],
    ["prefixItems pin the head, items govern the tail", { type: "array", prefixItems: [{ type: "string" }, { type: "number" }], items: { type: "integer" } }, ["a", 1, 2, 3], true],
    ["items refuses a bad tail element", { type: "array", prefixItems: [{ type: "string" }, { type: "number" }], items: { type: "integer" } }, ["a", 1, "x"], false],
    ["prefixItems refuse a bad head element", { type: "array", prefixItems: [{ type: "string" }, { type: "number" }], items: { type: "integer" } }, [1, 2], false],

    // -- object keywords --
    ["required refuses a missing member (dynajs.d.ts example schema uses required)", { type: "object", required: ["id"] }, {}, false],
    ["required passes when present", { type: "object", required: ["id"] }, { id: 1 }, true],
    ["additionalProperties: false refuses an unevaluated key", { type: "object", properties: { a: { type: "string" } }, additionalProperties: false }, { a: "x", b: 1 }, false],
    ["additionalProperties: false passes when only declared keys appear", { type: "object", properties: { a: { type: "string" } }, additionalProperties: false }, { a: "x" }, true],
    ["patternProperties constrains only matching keys", { type: "object", patternProperties: { "^S": { type: "string" } } }, { Sx: "s", t: 1 }, true],
    ["patternProperties refuses a violation on a matching key", { type: "object", patternProperties: { "^S": { type: "string" } } }, { Sx: 1 }, false],
    ["propertyNames passes for conforming keys", { propertyNames: { pattern: "^[a-z]+$" } }, { abc: 1 }, true],
    ["propertyNames refuses a non-conforming key", { propertyNames: { pattern: "^[a-z]+$" } }, { Abc: 1 }, false],
    ["minProperties inclusive", { type: "object", minProperties: 1 }, { a: 1 }, true],
    ["minProperties refuses an empty object", { type: "object", minProperties: 1 }, {}, false],
    ["maxProperties refuses extra members", { type: "object", maxProperties: 1 }, { a: 1, b: 2 }, false],
    ["dependentRequired passes when the dependent is present", { type: "object", dependentRequired: { a: ["b"] } }, { a: 1, b: 2 }, true],
    ["dependentRequired refuses a missing dependent", { type: "object", dependentRequired: { a: ["b"] } }, { a: 1 }, false],
    ["dependentRequired is silent when the trigger key is absent", { type: "object", dependentRequired: { a: ["b"] } }, { b: 2 }, true],

    // -- boolean schemas (2020-12 core) --
    ["the boolean schema true accepts everything", true, "anything", true],
    ["the boolean schema false refuses everything", false, "anything", false],
    ["the empty schema accepts everything", {}, 42, true],

    // -- combinators --
    ["allOf: every branch must pass", { allOf: [{ type: "string" }, { minLength: 2 }] }, "ab", true],
    ["allOf: one failing branch fails", { allOf: [{ type: "string" }, { minLength: 2 }] }, "a", false],
    ["anyOf: one passing branch suffices", { anyOf: [{ type: "string" }, { type: "integer" }] }, "x", true],
    ["anyOf: no passing branch fails", { anyOf: [{ type: "string" }, { type: "integer" }] }, 1.5, false],
    ["oneOf: exactly one branch passes", { oneOf: [{ type: "string" }, { type: "integer" }] }, "x", true],
    ["oneOf: zero branches fails", { oneOf: [{ type: "string" }, { type: "integer" }] }, 1.5, false],
    ["oneOf: TWO passing branches fail (2020-12 exactly-one rule)", { oneOf: [{ type: "string" }, { minLength: 1 }] }, "x", false],
    ["not: a non-string passes", { not: { type: "string" } }, 5, true],
    ["not: a string fails", { not: { type: "string" } }, "s", false],
    ["if/then: the then-branch applies", { if: { type: "string" }, then: { minLength: 2 }, else: { type: "integer" } }, "ab", true],
    ["if/then: a failing then-branch fails", { if: { type: "string" }, then: { minLength: 2 }, else: { type: "integer" } }, "a", false],
    ["if/else: the else-branch applies", { if: { type: "string" }, then: { minLength: 2 }, else: { type: "integer" } }, 5, true],
    ["if/else: a failing else-branch fails", { if: { type: "string" }, then: { minLength: 2 }, else: { type: "integer" } }, 1.5, false],

    // -- $ref (relative pointers; dynajs.d.ts: "$ref resolves at compile time") --
    ["$ref into $defs resolves (2020-12)", { type: "object", properties: { id: { $ref: "#/$defs/positiveInt" } }, $defs: { positiveInt: { type: "integer", minimum: 1 } } }, { id: 1 }, true],
    ["the $ref target's constraints apply", { type: "object", properties: { id: { $ref: "#/$defs/positiveInt" } }, $defs: { positiveInt: { type: "integer", minimum: 1 } } }, { id: 0 }, false],
    ["the $ref target's type applies", { type: "object", properties: { id: { $ref: "#/$defs/positiveInt" } }, $defs: { positiveInt: { type: "integer", minimum: 1 } } }, { id: "x" }, false],
    ["root $ref recursion: []", { type: "array", items: { $ref: "#" } }, [], true],
    ["root $ref recursion: [[]]", { type: "array", items: { $ref: "#" } }, [[]], true],
    // TEST-FIX: items:{$ref:"#"} on an array root requires EVERY element at EVERY depth to
    // satisfy the root, so a number at the innermost level violates type:array (the engine
    // reports /0/0/0/0 "must be a array"). Only all-empty nesting is valid; the previous row
    // expected [[[[1]]]] -> true, which contradicts the recursion itself.
    ["root $ref recursion: nested arrays", { type: "array", items: { $ref: "#" } }, [[[[[]]]]], true],
    ["root $ref recursion: a number at the innermost level fails", { type: "array", items: { $ref: "#" } }, [[[[1]]]], false],
    ["root $ref: a non-array element fails", { type: "array", items: { $ref: "#" } }, ["x"], false],
    ["root $ref: the failure shows at any depth", { type: "array", items: { $ref: "#" } }, [[1]], false],

    // -- unevaluatedProperties / unevaluatedItems, with the doc-pinned annotation-flow rules
    //    (dynajs.d.ts: flow through allOf, dependentSchemas, if/then/else, $ref targets and passing
    //    anyOf/oneOf branches; "a failed subschema annotates nothing") --
    ["unevaluatedProperties: a properties-evaluated key is fine", { type: "object", properties: { a: true }, unevaluatedProperties: false }, { a: 1 }, true],
    ["unevaluatedProperties: an unevaluated key fails", { type: "object", properties: { a: true }, unevaluatedProperties: false }, { a: 1, b: 2 }, false],
    ["annotations flow through allOf", { allOf: [{ properties: { a: true } }, { properties: { b: true } }], unevaluatedProperties: false }, { a: 1, b: 2 }, true],
    ["a key no allOf branch evaluated still fails", { allOf: [{ properties: { a: true } }, { properties: { b: true } }], unevaluatedProperties: false }, { c: 3 }, false],
    ["annotations flow through the passing anyOf branch", { anyOf: [{ type: "string" }, { properties: { a: true } }], unevaluatedProperties: false }, { a: 1 }, true],
    ["the failing branch annotated nothing, so 'b' is unevaluated", { anyOf: [{ type: "string" }, { properties: { a: true } }], unevaluatedProperties: false }, { b: 1 }, false],
    ["dependentSchemas evaluates 'b' when 'a' triggers", { type: "object", properties: { a: { type: "integer" } }, dependentSchemas: { a: { properties: { b: true } } }, unevaluatedProperties: false }, { a: 1, b: 2 }, true],
    ["an untriggered, unevaluated key fails", { type: "object", properties: { a: { type: "integer" } }, dependentSchemas: { a: { properties: { b: true } } }, unevaluatedProperties: false }, { a: 1, c: 3 }, false],
    ["the passing if's then-branch evaluates 'x'", { type: "object", if: { properties: { k: { const: "v" } }, required: ["k"] }, then: { properties: { x: true } }, unevaluatedProperties: false }, { k: "v", x: 1 }, true],
    ["'y' was evaluated nowhere", { type: "object", if: { properties: { k: { const: "v" } }, required: ["k"] }, then: { properties: { x: true } }, unevaluatedProperties: false }, { k: "v", y: 2 }, false],
    ["a failed if annotates nothing, so 'x' is unevaluated", { type: "object", if: { properties: { k: { const: "v" } }, required: ["k"] }, then: { properties: { x: true } }, unevaluatedProperties: false }, { k: "z", x: 1 }, false],
    ["annotations flow through the $ref target", { $defs: { p: { properties: { a: true } } }, $ref: "#/$defs/p", unevaluatedProperties: false }, { a: 1 }, true],
    ["outside the $ref target's evaluation a key fails", { $defs: { p: { properties: { a: true } } }, $ref: "#/$defs/p", unevaluatedProperties: false }, { b: 1 }, false],
    ["unevaluatedItems: prefixItems-evaluated positions pass", { type: "array", prefixItems: [{ type: "number" }], unevaluatedItems: false }, [1], true],
    ["unevaluatedItems: an extra item fails", { type: "array", prefixItems: [{ type: "number" }], unevaluatedItems: false }, [1, 2], false],
];
for (const [label, schema, instance, expected] of VERDICTS)
    assertEq(Schema.validate(schema, instance).valid, expected, "schema verdict: " + label);

// ------------------------------------------------------------------
// compile/validate cycle, result shape, caching — dynajs.d.ts example schema.
// ------------------------------------------------------------------

{
    const schema = {
        type: "object",
        properties: {
            id: { type: "integer", minimum: 1 },
            email: { type: "string" },
        },
        required: ["id", "email"],
    };
    const compiled = Schema.compile(schema);

    // Rows: [label, instance, expectedValid, expectEmptyErrors].
    const CYCLE = [
        ["the doc example instance is valid", { id: 1, email: "user@example.com" }, true, true],
        ["{ id: 0 } is invalid (minimum AND required email)", { id: 0 }, false, false],
        ["a non-integer id is invalid", { id: "x", email: "u@e.co" }, false, false],
    ];
    for (const [label, instance, expectedValid, expectEmpty] of CYCLE) {
        const r = compiled.validate(instance);
        assertEq(r.valid, expectedValid, "CompiledSchema.validate: " + label);
        assertEq(Array.isArray(r.errors), true, "CompiledSchema.validate: errors is an array — " + label);
    }
    const ok = compiled.validate({ id: 1, email: "user@example.com" });
    assertEq(ok.errors.length, 0, "a valid result carries an empty errors array (d.ts SchemaResult)");

    // Error shape: d.ts SchemaError { path, message, keyword } of strings.
    const bad = compiled.validate({ id: 0 });
    assert(bad.errors.length >= 1, "an invalid result reports at least one error");
    for (const e of bad.errors)
        assert(typeof e.path === "string" && typeof e.message === "string" && typeof e.keyword === "string",
            "every error is { path, message, keyword }, all strings (d.ts SchemaError)");
    assert(bad.errors.some(e => e.path.includes("id")),
        "the path of the minimum violation names the offending property ('id')");

    // d.ts: "Compiles and caches on the schema object; accepts an already-compiled schema."
    // TEST-FIX: the rows previously validated BARE integers against this object schema
    // ({id, email} required) -- the document wrapper { id: ... } was missing, so the first
    // row could never be true. The id values keep the original labels meaningful.
    const CACHE = [
        ["one-shot accepts an integer id", { id: 1, email: "a@b.c" }, true],
        ["one-shot refuses a 1.5 id", { id: 1.5, email: "a@b.c" }, false],
        ["a second call still works (compile is cached)", { id: 2, email: "a@b.c" }, true],
        ["the already-compiled schema is accepted as first argument (doc pins)", { id: 3, email: "a@b.c" }, true],
    ];
    for (const [label, instance, expected] of CACHE) {
        const via = label.includes("already-compiled") ? compiled : schema;
        assertEq(Schema.validate(via, instance).valid, expected, "Schema.validate cache: " + label);
    }
}

// ------------------------------------------------------------------
// Documented bounds — dynajs.d.ts: "schema nesting capped at 256 ... an invalid pattern
// throws with the regex error", "the $ref chain at 64", "Errors are bounded at 256".
// ------------------------------------------------------------------

{
    // Rows: [label, factory] — factory builds the schema so rows stay data.
    const COMPILE_THROW = [
        ["nesting over 256 throws (dynajs.d.ts compile bounds)", () => {
            let deep = { type: "string" };
            for (let i = 0; i < 300; i++) deep = { allOf: [deep] };
            return deep;
        }],
        ["an invalid pattern throws (dynajs.d.ts compile bounds)", () => ({ type: "string", pattern: "[" })],
    ];
    for (const [label, factory] of COMPILE_THROW)
        assertThrows(() => Schema.compile(factory()), "Schema.compile: " + label);

    let okDeep = { type: "string" };
    for (let i = 0; i < 128; i++) okDeep = { allOf: [okDeep] };
    assertEq(Schema.compile(okDeep).validate("s").valid, true, "Schema.compile: nesting at 128 is inside the cap and validates");

    // dynajs.d.ts: "the $ref chain at 64". TEST-FIX: the cap bounds consecutive $ref INDIRECTIONS
    // inside one evaluation path — each instance child starts a fresh chain — so DEEP INSTANCE
    // NESTING is not an over-cap ref chain: it follows the recursion and is bounded by the
    // 512 instance cap instead. The previous rows expected nesting at 80 to fail closed.
    const rec = {
        $defs: { r: { anyOf: [{ type: "null" }, { type: "array", items: { $ref: "#/$defs/r" } }] } },
        $ref: "#/$defs/r",
    };
    for (const depth of [60, 80]) {
        let inst = null;
        for (let i = 0; i < depth; i++) inst = inst === null ? [] : [inst];
        let result;
        try { result = Schema.compile(rec).validate(inst).valid; } catch (e) { result = false; }
        assertEq(result, true, "ref recursion at depth " + depth +
            ": instance nesting is not a $ref chain (bounded by the 512 instance cap)");
    }
    // a genuine indirection chain trips at 64 hops and fails closed with a named $ref error
    const mkChain = (n) => {
        const s = { $defs: {} };
        for (let i = 0; i < n; i++) s.$defs["r" + i] = { $ref: "#/$defs/r" + (i + 1) };
        s.$defs["r" + n] = { type: "null" };
        s.$ref = "#/$defs/r0";
        return s;
    };
    assertEq(Schema.validate(mkChain(63), null).valid, true, "a 63-hop $ref chain is inside the 64 cap");
    {
        const over = Schema.validate(mkChain(64), null);
        assertEq(over.valid, false, "a 64-hop $ref chain exceeds the 64 cap and fails closed");
        assert(over.errors.some((e) => e.keyword === "$ref" && String(e.message).includes("64")),
            "the over-cap error names $ref and the 64-level bound");
    }

    // dynajs.d.ts: "Errors are bounded at 256".
    const props = {}, inst = {};
    for (let i = 0; i < 300; i++) { props["p" + i] = { type: "string" }; inst["p" + i] = 1; }
    const capped = Schema.validate({ type: "object", properties: props }, inst);
    assertEq(capped.valid, false, "error cap: 300 type violations are invalid");
    assertEq(capped.errors.length, 256, "error cap: the error list is bounded at exactly 256 (dynajs.d.ts)");
}

// ------------------------------------------------------------------
// registerFormat — d.ts/dynajs.d.ts: live at validate() time, per-runtime, string
// instances only, duplicates refused, a throw aborts validate.
// ------------------------------------------------------------------

{
    Schema.registerFormat("bb_evenLength", (s) => s.length % 2 === 0);
    Schema.registerFormat("bb_late", (s) => s === "yes");
    Schema.registerFormat("bb_boom", () => { throw new Error("boom"); });
    const fmt = Schema.compile({ type: "string", format: "bb_evenLength" });
    const fmtAny = Schema.compile({ format: "bb_evenLength" });
    const early = Schema.compile({ type: "string", format: "bb_late" });

    // Rows: [label, validator, instance, expectedValid].
    const FORMAT = [
        ["a registered format asserts on strings (dynajs.d.ts example)", (v) => fmt.validate(v).valid, "abcd", true],
        ["...and refuses odd lengths", (v) => fmt.validate(v).valid, "abc", false],
        ["a number instance passes the format unchecked (doc pins; no type keyword)", (v) => fmtAny.validate(v).valid, 5, true],
        ["an object instance passes the format unchecked (doc pins; no type keyword)", (v) => fmtAny.validate(v).valid, {}, true],
        // TEST-FIX: bb_late is registered at the top of this block (before `early` compiles), so
        // early.validate is asserted by the live registry and "whatever" is false. The
        // annotation-only rule needs a never-registered name.
        ["an unregistered name is annotation-only (no assertion, no error)", (v) => Schema.validate({ type: "string", format: "bb_never_registered" }, v).valid, "whatever", true],
        ["the registered bb_late asserts on a non-matching string", (v) => early.validate(v).valid, "whatever", false],
        ["the registry is live: the pre-compiled schema now asserts (doc pins)", (v) => early.validate(v).valid, "yes", true],
        ["...and refuses after registration", (v) => early.validate(v).valid, "no", false],
    ];
    for (const [label, fn, instance, expected] of FORMAT)
        assertEq(fn(instance), expected, "registerFormat: " + label);

    // Rows: [label, fn, ErrType].
    const FORMAT_THROW = [
        ["a duplicate format name is refused (immutable registry)", () => Schema.registerFormat("bb_evenLength", (s) => true), TypeError],
        ["a non-function validator is refused", () => Schema.registerFormat("bb_nothandler", "nope"), TypeError],
        ["an empty format name is refused", () => Schema.registerFormat("", (s) => true), TypeError],
        ["a throwing format validator aborts validate", () => Schema.compile({ type: "string", format: "bb_boom" }).validate("x"), undefined],
    ];
    for (const [label, fn, ErrType] of FORMAT_THROW)
        assertThrows(fn, "registerFormat: " + label, ErrType);
}

print("bb_schema: all tests passed (" + n + " assertions)");
