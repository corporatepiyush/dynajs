import { Schema } from "dyna:schema";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function v(s, i) { try { return Schema.validate(s, i).valid === true; } catch (e) { return "THREW:" + String(e); } }
function ok(s, i, msg) { assert(v(s, i) === true, msg + " (expected valid)"); }
function no(s, i, msg) { assert(v(s, i) === false, msg + " (expected invalid)"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

ok({ contains: { minimum: 5 } }, [3, 4, 5], "contains: one matching element");
no({ contains: { minimum: 5 } }, [2, 3, 4], "contains: no matching element");
no({ contains: { minimum: 5 } }, [], "contains: empty array fails (default min 1)");
ok({ contains: { minimum: 5 } }, {}, "contains: ignored on objects");
ok({ contains: true }, ["foo"], "contains: true schema matches everything");
no({ contains: true }, [], "contains: true schema still needs one element");
no({ contains: false }, ["foo"], "contains: false schema matches nothing");
no({ contains: false }, [], "contains: false schema on empty array");
ok({ contains: { type: "null" } }, [null], "contains: null elements");

ok({ contains: { const: 1 }, minContains: 2 }, [1, 1], "minContains 2 satisfied");
no({ contains: { const: 1 }, minContains: 2 }, [1], "minContains 2 unmet");
no({ contains: { const: 1 }, minContains: 2 }, [1, 2], "minContains counts matches only");
ok({ contains: { const: 1 }, minContains: 2.0 }, [1, 1], "minContains accepts integral decimals");
ok({ contains: { const: 1 }, minContains: 0 }, [2], "minContains 0 passes without matches");
no({ contains: { const: 1 }, minContains: 0, maxContains: 0 }, [1],
   "minContains 0 with maxContains 0 rejects a match");
ok({ minContains: 1 }, [], "minContains without contains is ignored");
no({ contains: { const: 1 }, maxContains: 1 }, [1, 1], "maxContains exceeded");
ok({ contains: { const: 1 }, maxContains: 1.0 }, [1], "maxContains accepts integral decimals");
ok({ contains: { const: 1 }, maxContains: 1 }, [1, 2], "maxContains counts matching elements only");
ok({ maxContains: 1 }, [1, 2], "maxContains without contains is ignored");
ok({ contains: { const: 1 }, maxContains: 2, minContains: 2 }, [1, 1],
   "maxContains == minContains");
no({ contains: { const: 1 }, maxContains: 1, minContains: 2 }, [1, 1],
   "maxContains < minContains can never pass");
no({ contains: { const: 1 }, minContains: 1 }, [], "contains+minContains 1 on []");
throws(() => Schema.compile({ contains: {} , minContains: -1 }),
       "negative minContains is refused at compile");
throws(() => Schema.compile({ contains: {}, maxContains: 1.5 }),
       "fractional maxContains is refused at compile");
{
    const r = Schema.validate({ contains: { type: "integer" } }, ["a", "b"]);
    assert(r.valid === false, "contains failure is invalid");
    assert(r.errors.length >= 1 && r.errors.every((e) =>
        e.keyword === "contains"), "contains reports its own keyword only");
}

ok({ propertyNames: { maxLength: 3 } }, { foo: 1 }, "propertyNames within bound");
no({ propertyNames: { maxLength: 3 } }, { foobar: 1 }, "propertyNames over bound");
ok({ propertyNames: { maxLength: 3 } }, {}, "propertyNames on empty object");
ok({ propertyNames: { maxLength: 3 } }, [1, 2, 3, 4], "propertyNames ignores arrays");
ok({ propertyNames: { pattern: "^a+$" } }, { aa: 1 }, "propertyNames pattern");
no({ propertyNames: { pattern: "^a+$" } }, { aaA: 1 }, "propertyNames pattern mismatch");
no({ propertyNames: false }, { foo: 1 }, "propertyNames false rejects any prop");
ok({ propertyNames: false }, {}, "propertyNames false accepts empty object");
ok({ propertyNames: { const: "foo" } }, { foo: 1 }, "propertyNames const");
ok({ propertyNames: { enum: ["foo", "bar"] } }, { foo: 1, bar: 2 }, "propertyNames enum");
no({ propertyNames: { enum: ["foo", "bar"] } }, { baz: 1 }, "propertyNames enum mismatch");
{
    const r = Schema.validate({ propertyNames: { maxLength: 3 } }, { foobar: 1 });
    assert(r.errors.length >= 1 && r.errors.some((e) => e.path === "/foobar"),
           "propertyNames error reports the offending name as the path");
}
no({ properties: { a: { type: "integer" } }, propertyNames: { maxLength: 1 } },
   { bb: "x" }, "propertyNames + properties: both apply");

ok({ dependentSchemas: { bar: { required: ["foo"] } } }, { foo: 1, bar: 2 },
   "dependentSchemas: trigger present, schema satisfied");
no({ dependentSchemas: { bar: { required: ["foo"] } } }, { bar: 2 },
   "dependentSchemas: trigger present, schema violated");
ok({ dependentSchemas: { bar: { required: ["foo"] } } }, {},
   "dependentSchemas: trigger absent");
ok({ dependentSchemas: { bar: { required: ["foo"] } } }, ["bar"],
   "dependentSchemas: ignored on arrays");
ok({ dependentSchemas: { bar: { required: ["foo"] } } }, "bar",
   "dependentSchemas: ignored on strings");
no({ dependentSchemas: { foo: true, bar: false } }, { bar: 1 },
   "dependentSchemas: boolean false subschema rejects");
ok({ dependentSchemas: { foo: true, bar: false } }, { foo: 1 },
   "dependentSchemas: boolean true subschema passes");
no({ properties: { foo: {} },
     dependentSchemas: { foo: { properties: { bar: { type: "string" } },
                                required: ["bar"] } } },
   { foo: 1 }, "dependentSchemas composes with root properties");
{
    const r = Schema.validate(
        { dependentSchemas: { a: { required: ["b"] } } }, { a: 1 });
    assert(r.valid === false && r.errors.length >= 1,
           "dependentSchemas failure surfaces the dependent schema's error");
}
no({ dependentSchemas: { t: { properties: { u: { type: "integer" } }, required: ["u"] } } },
   { t: { u: 1 } }, "dependentSchemas: required targets the whole instance");
ok({ $defs: { r: { dependentSchemas: { t: { properties: { t: { $ref: "#/$defs/r" } } } } } },
     $ref: "#/$defs/r" },
   { t: { t: {} } }, "descending recursive dependentSchemas terminates");
{
    const s = { $defs: { r: { dependentSchemas: { t: { $ref: "#/$defs/r" } } } }, $ref: "#/$defs/r" };
    const r = Schema.validate(s, { t: { t: {} } });
    assert(r.valid === false && r.errors.some((e) => e.keyword === "$ref"),
           "same-instance dependentSchemas cycle fails closed at the bound");
}
{
    const r = Schema.validate({ anyOf: [{ contains: { const: 9 } }, { type: "string" }] }, [1, 2]);
    assert(r.valid === false && r.errors.length === 1 &&
           r.errors[0].keyword === "contains",
           "anyOf tie surfaces the first-best branch's single contains error");
}

{
    let inst = [], cur = inst;
    for (let i = 0; i < 300; i++) { const nx = []; cur.push(nx); cur = nx; }
    const s = { $defs: { a: { items: { $ref: "#/$defs/a" } } }, $ref: "#/$defs/a" };
    try {
        Schema.validate(s, inst);
        assert(false, "deep instance must throw past the runtime cap");
    } catch (e) {
        assert(e instanceof Error && typeof e.message === "string" &&
               e.message.indexOf("nesting") >= 0,
               "a failing first Schema.validate surfaces a real Error, got: "
               + typeof e + " " + String(e));
    }
}
{
    try {
        Schema.validate({ pattern: "(a+)+$" }, "a".repeat(30) + "b");
        assert(false, "regex budget exhaustion must throw");
    } catch (e) {
        assert(e instanceof Error,
               "regex failure surfaces a real Error, got: " + typeof e);
    }
}

{
    const s = { type: "integer" };
    assert(Schema.validate(s, 1).valid === true, "cache: first call");
    assert(Schema.validate(s, "x").valid === false, "cache: second call reuses rules");
    s.maximum = 5;
    assert(Schema.validate(s, 10).valid === false,
           "cache: mutation invalidates (new rules apply)");
    delete s.maximum;
    assert(Schema.validate(s, 10).valid === true, "cache: revert re-invalidates cache");
}
{
    const a = { type: "string" };
    const b = { type: "string" };
    assert(Schema.validate(a, "x"), "cache: object A");
    assert(Schema.validate(b, "y"), "cache: object B (no cross-object cache)");
    a.type = "integer";
    assert(Schema.validate(b, "y").valid === true,
           "cache: B unaffected by A's mutation");
}
{
    const s = Object.freeze({ type: "integer" });
    assert(Schema.validate(s, 1).valid === true, "freeze: validates");
    assert(Schema.validate(s, "x").valid === false, "freeze: still enforces");
    assert(Object.isFrozen(s), "freeze: schema object untouched");
    assert(s.__dyna_compiled_schema === undefined,
           "freeze: no cache property leaked onto a frozen schema");
}
{
    const s = { type: "integer" };
    Schema.validate(s, 1);
    const keys = Object.keys(s);
    assert(keys.length === 1 && keys[0] === "type",
           "cache: only schema keywords are enumerable");
    const names = Object.getOwnPropertyNames(s);
    assert(names.indexOf("__dyna_compiled_schema") >= 0 &&
           names.indexOf("__dyna_compiled_hash") >= 0,
           "cache: compiled schema stored non-enumerably");
    assert(s.type === "integer", "cache: schema document unchanged");
}
{
    const c = Schema.compile({ type: "integer" });
    assert(Schema.validate(c, 1).valid === true, "compiled accepted by Schema.validate");
    assert(Schema.validate(c, "x").valid === false, "compiled enforces");
    assert(c.validate(2).valid === true, "compiled.validate works");
}
{
    assert(Schema.compile('{"type":"integer"}').validate(1).valid === true,
           "compile from a JSON string");
}

throws(() => Schema.compile({ $dynamicRef: "#/$defs/x", $defs: { x: {} } }),
       "$dynamicRef is refused at compile (applicator, not ignored)");
throws(() => Schema.compile({ $defs: { a: { $ref: "#/$defs/b" }, b: { $ref: "#/$defs/a" } }, $ref: "#/$defs/a" }),
       "pure ref cycle refused at compile");
throws(() => Schema.compile({ $ref: "#" }), "self ref cycle refused at compile");
{
    let s = { $defs: { r0: { type: "integer" } } };
    for (let i = 1; i < 40; i++) s.$defs["r" + i] = { $ref: "#/$defs/r" + (i - 1) };
    s.$ref = "#/$defs/r39";
    assert(v(s, 3) === true, "40-deep terminating ref chain validates");
    let big = { $defs: { r0: { type: "integer" } } };
    for (let i = 1; i < 100; i++) big.$defs["r" + i] = { $ref: "#/$defs/r" + (i - 1) };
    big.$ref = "#/$defs/r99";
    const r = Schema.validate(big, 3);
    assert(r.valid === false, "over-cap ref chain fails closed");
    assert(r.errors.some((e) => e.keyword === "$ref" &&
                             e.message.indexOf("indirection exceeds") >= 0),
           "over-cap ref chain reports the cap, not circularity");
}
{
    const s = { $ref: "#/$defs/pos", maximum: 5, $defs: { pos: { type: "integer" } } };
    assert(v(s, 3) === true, "$ref siblings: ref alone passes");
    assert(v(s, 9) === false, "$ref siblings: sibling maximum applies");
    assert(v(s, "x") === false, "$ref siblings: ref type applies");
}
throws(() => Schema.compile(false ? null : deepAllOf(257)), "schema depth cap 256");
function deepAllOf(d) {
    let s = { type: "integer" };
    for (let i = 0; i < d; i++) s = { allOf: [s] };
    return s;
}
ok(deepAllOf(255), 1, "schema depth 255 compiles");
throws(() => Schema.compile({ enum: list(1 << 20 + 1) }), "enum over 2^20 refused");
throws(() => Schema.compile({ type: list(1 << 20 + 1) }), "type array over 2^20 refused");
throws(() => Schema.compile({ pattern: "a".repeat(64 * 1024 + 1) }),
       "pattern source cap 64KiB");
function list(k) { const a = []; for (let i = 0; i < k; i++) a.push("x"); return a; }
{
    const s = { $defs: { a: { items: { $ref: "#/$defs/a" } } }, $ref: "#/$defs/a" };
    let inst = [], cur = inst;
    for (let i = 0; i < 513; i++) { const nx = []; cur.push(nx); cur = nx; }
    throws(() => Schema.compile(s) && Schema.validate(s, inst),
           "instance nesting past 512 throws (fail-closed, not invalid)");
}
{
    const s = { $defs: { n: { type: "object", properties: { next: { $ref: "#/$defs/n" } } } },
                $ref: "#/$defs/n" };
    let d = {}, cur = d;
    for (let i = 0; i < 100; i++) { cur.next = {}; cur = cur.next; }
    ok(s, d, "100-deep recursive instance terminates");
}

ok({ minimum: 0 }, -0, "-0 satisfies minimum 0");
no({ exclusiveMaximum: 0 }, -0, "-0 == 0 fails an exclusive maximum of 0");
ok({ type: "integer" }, 1.0, "1.0 is an integer");
ok({ type: "integer" }, 9007199254740992, "2^53 is an integer");
ok({ maximum: 9007199254740993 }, 9007199254740993,
   "2^53+1: both sides collapse to the same double, consistent");
ok({ multipleOf: 1 }, 9007199254740993, "2^53+1 is a multiple of 1");
ok({ multipleOf: 0.1 }, 0.3, "0.3 is a multiple of 0.1 (epsilon tolerance)");
no({ multipleOf: 0.123456789 }, 1e308, "1e308 vs fractional multiple (overflow path)");
no({ minimum: 0 }, NaN, "NaN satisfies no numeric bound");
throws(() => Schema.compile({ minimum: "5" }), "minimum rejects a string");
throws(() => Schema.compile({ minimum: true }), "minimum rejects a boolean");
throws(() => Schema.compile({ minLength: "3" }), "minLength rejects a string");
throws(() => Schema.compile({ maxItems: null }), "maxItems rejects null");
ok({ uniqueItems: true }, [1, true], "uniqueItems: 1 and true are distinct");
ok({ uniqueItems: true }, [0, false], "uniqueItems: 0 and false are distinct");
ok({ uniqueItems: true }, ["1", 1], "uniqueItems: '1' and 1 are distinct");
no({ uniqueItems: true }, [null, null], "uniqueItems: null duplicates");
no({ uniqueItems: true }, [[1], [1]], "uniqueItems: deep array duplicates");
ok({ uniqueItems: true }, [NaN, NaN], "uniqueItems: NaN never equals NaN (===)");
{
    const sch = JSON.parse('{"properties":{"__proto__":{"type":"integer"}}}');
    ok(sch, JSON.parse('{"__proto__":1}'), "__proto__ own key validated as data");
    no(sch, JSON.parse('{"__proto__":"x"}'), "__proto__ own key type-checked");
    no({ required: ["__proto__"] }, {}, "required __proto__ on a plain object");
}
{
    const inst = JSON.parse('{"a\\u0000b":1}');
    const r = Schema.validate({ properties: {} , additionalProperties: false }, inst);
    assert(r.valid === false, "NUL property: additionalProperties anchored at parent");
}
{
    const inst = JSON.parse('{"a\\u0000b":"x"}');
    const r = Schema.validate({ properties: {} }, inst);
    assert(r.valid === true, "NUL property name: no crash on untouched path");
}
{
    const inst = JSON.parse('{"a\\u0000b":"x"}');
    const r = Schema.validate({ additionalProperties: false }, inst);
    assert(r.errors.length === 1 && r.errors[0].path === "",
           "additionalProperties error anchors at the object (ajv-style)");
}
{
    const inst = JSON.parse('{"a/b":1}');
    const r = Schema.validate({ propertyNames: { maxLength: 0 } }, inst);
    assert(r.errors.some((e) => e.path === "/a~1b"),
           "propertyNames path escapes '/' per RFC 6901");
}
{
    const big = [];
    for (let i = 0; i < 10000; i++) big.push("k" + i);
    const inst = {};
    for (let i = 0; i < 10000; i++) inst["k" + i] = i;
    ok({ required: big }, inst, "required 10k satisfied");
    no({ required: big }, {}, "required 10k unsatisfied");
}
{
    const r = Schema.validate({ required: ["a", "a"] }, { a: 1 });
    assert(r.valid === true, "duplicate required present");
    const r2 = Schema.validate({ required: ["a", "a"] }, {});
    assert(r2.valid === false, "duplicate required missing");
}

{
    no({ unevaluatedProperties: false }, { foo: 1 }, "up: bare false");
    ok({ unevaluatedProperties: false }, {}, "up: bare false empty");
    no({ properties: { foo: {} }, unevaluatedProperties: false },
       { foo: 1, bar: 2 }, "up: adjacent properties");
    ok({ properties: { foo: {} }, allOf: [{ properties: { bar: {} } }],
         unevaluatedProperties: false },
       { foo: 1, bar: 2 }, "up: allOf branch evaluations count");
    no({ properties: { foo: {} }, allOf: [{ properties: { bar: {} } }],
         unevaluatedProperties: false },
       { foo: 1, bar: 2, baz: 3 }, "up: allOf does not cover a third prop");
    no({ allOf: [{ properties: { foo: {} } }, { unevaluatedProperties: false }] },
       { foo: 1 }, "up: cousins are invisible to each other");
    ok({ properties: { foo: {} },
         anyOf: [{ properties: { bar: { const: 2 } }, required: ["bar"] }],
         unevaluatedProperties: false },
       { foo: 1, bar: 2 }, "up: passing anyOf branch annotates");
    no({ properties: { foo: {} },
         anyOf: [{ properties: { bar: { const: 2 } }, required: ["bar"] }],
         unevaluatedProperties: false },
       { foo: 1, bar: 3 }, "up: failing anyOf branch does not annotate");
    ok({ if: { properties: { foo: { const: "then" } }, required: ["foo"] },
         then: { properties: { bar: {} }, required: ["bar"] },
         else: { properties: { baz: {} }, required: ["baz"] },
         unevaluatedProperties: false },
       { foo: "then", bar: 1 }, "up: then-branch annotations");
    ok({ if: { properties: { foo: { const: "then" } }, required: ["foo"] },
         then: { properties: { bar: {} }, required: ["bar"] },
         else: { properties: { baz: {} }, required: ["baz"] },
         unevaluatedProperties: false },
       { baz: "x" }, "up: else-branch annotations");
    no({ if: { properties: { foo: { const: "then" } }, required: ["foo"] },
         then: { properties: { bar: {} }, required: ["bar"] },
         else: { properties: { baz: {} }, required: ["baz"] },
         unevaluatedProperties: false },
       { foo: "else", baz: "x" }, "up: failed if annotates nothing");
    ok({ if: { patternProperties: { foo: {} } }, unevaluatedProperties: false },
       { foo: 1 }, "up: passing if without then/else annotates");
    ok({ properties: { foo2: {} },
         dependentSchemas: { foo2: { properties: { bar: {} } } },
         unevaluatedProperties: false },
       { foo2: 1, bar: 2 }, "up: dependentSchemas annotations");
    no({ properties: { foo2: {} },
         dependentSchemas: { foo2: { properties: { bar: {} } } },
         unevaluatedProperties: false },
       { bar: 2 }, "up: untriggered dependentSchemas annotates nothing");
    ok({ $ref: "#/$defs/b", properties: { foo: {} },
         unevaluatedProperties: false,
         $defs: { b: { properties: { bar: {} } } } },
       { foo: 1, bar: 2 }, "up: $ref target annotates");
    no({ required: ["foo"], unevaluatedProperties: false }, { foo: 1 },
       "up: required does not evaluate");
    no({ propertyNames: { maxLength: 5 }, unevaluatedProperties: false },
       { foo: 1 }, "up: propertyNames does not evaluate the property");
    ok({ propertyNames: { maxLength: 5 },
         unevaluatedProperties: { type: "number" } },
       { foo: 1 }, "up: unevaluatedProperties itself evaluates the value");
    no({ properties: { foo: { properties: { bar: {} } } },
         unevaluatedProperties: false },
       { foo: { bar: 1 }, bar: 2 }, "up: instance-location isolation");
    ok({ properties: { x: { $ref: "#" } }, unevaluatedProperties: false },
       { x: { x: {} } }, "up: cyclic $ref valid depth");
    no({ properties: { x: { $ref: "#" } }, unevaluatedProperties: false },
       { x: { x: {}, y: 1 } }, "up: cyclic $ref offending depth");
    ok({ allOf: [{ properties: { foo: {} }, unevaluatedProperties: true }],
         unevaluatedProperties: false },
       { foo: 1, bar: 2 }, "up: inner unevaluated-true annotates");
    {
        const r = Schema.validate({ unevaluatedProperties: false }, { a: 1 });
        assert(r.valid === false && r.errors.length >= 1 &&
               r.errors[0].path === "" &&
               r.errors[0].keyword === "unevaluatedProperties",
               "up: boolean-false error anchors at the object path");
    }
    {
        const r = Schema.validate(
            { unevaluatedProperties: { type: "string" } }, { a: 1 });
        assert(r.valid === false &&
               r.errors.some((e) => e.path === "/a" && e.keyword === "type"),
               "up: subschema errors anchor at the child path");
    }
    ok({ unevaluatedProperties: false }, "s", "up: non-object instance");
    throws(() => Schema.validate({ unevaluatedProperties: 3 }, {}),
           "up: non-boolean/schema keyword refused at compile");

    no({ unevaluatedItems: false }, [1], "ui: bare false");
    ok({ unevaluatedItems: false }, [], "ui: bare false empty");
    ok({ prefixItems: [{ type: "string" }], unevaluatedItems: false },
       ["a"], "ui: prefix item evaluated");
    no({ prefixItems: [{ type: "string" }], unevaluatedItems: false },
       ["a", 1], "ui: beyond prefix unevaluated");
    ok({ items: { type: "number" }, unevaluatedItems: false },
       [1, 2, 3], "ui: items evaluates all");
    ok({ prefixItems: [{ type: "string" }],
         allOf: [{ prefixItems: [true, { type: "number" }] }],
         unevaluatedItems: false },
       ["a", 42], "ui: allOf prefixItems merges");
    no({ prefixItems: [{ type: "string" }],
         allOf: [{ prefixItems: [true, { type: "number" }] }],
         unevaluatedItems: false },
       ["a", 42, true], "ui: third item unevaluated");
    ok({ prefixItems: [true], contains: { type: "string" },
         unevaluatedItems: false },
       [1, "foo"], "ui: contains match evaluates");
    no({ prefixItems: [true], contains: { type: "string" },
         unevaluatedItems: false },
       [1, 2, "foo"], "ui: contains leaves the gap unevaluated");
    ok({ contains: { type: "string" }, minContains: 0,
         unevaluatedItems: false },
       ["foo", "bar"], "ui: minContains 0, all matched");
    no({ contains: { type: "string" }, minContains: 0,
         unevaluatedItems: false },
       ["foo", 0], "ui: minContains 0, unmatched tail");
    ok({ allOf: [{ contains: { multipleOf: 2 } },
                 { contains: { multipleOf: 3 } }],
         unevaluatedItems: { multipleOf: 5 } },
       [2, 3, 4, 5, 6], "ui: nested contains merge");
    no({ allOf: [{ contains: { multipleOf: 2 } },
                 { contains: { multipleOf: 3 } }],
         unevaluatedItems: { multipleOf: 5 } },
       [2, 3, 4, 7, 8], "ui: nested contains, unevaluated offender");
    no({ if: { contains: { const: "a" } }, unevaluatedItems: false },
       ["b"], "ui: failing if contains annotates nothing");
    ok({ if: { contains: { const: "a" } }, unevaluatedItems: false },
       ["a"], "ui: passing if contains annotates");
    ok({ prefixItems: [{ const: "foo" }],
         anyOf: [{ prefixItems: [true, { const: "bar" }] },
                 { prefixItems: [true, true, { const: "baz" }] }],
         unevaluatedItems: false },
       ["foo", "bar", "baz"], "ui: two passing anyOf branches annotate");
    no({ prefixItems: [{ const: "foo" }],
         anyOf: [{ prefixItems: [true, { const: "bar" }] },
                 { prefixItems: [true, true, { const: "baz" }] }],
         unevaluatedItems: false },
       ["foo", "bar", 42], "ui: anyOf cannot cover a third item");
    no({ properties: { foo: { prefixItems: [{ type: "string" }],
                              unevaluatedItems: false } },
         anyOf: [{ properties: { foo: { prefixItems: [true,
                     { type: "string" }] } } }] },
       { foo: ["test", "test"] }, "ui: uncle evaluation not significant");
    ok({ unevaluatedItems: false }, 42, "ui: non-array instance");
    throws(() => Schema.validate({ unevaluatedItems: "x" }, []),
           "ui: non-boolean/schema keyword refused at compile");

    {
        const big = {};
        for (let i = 0; i < 5000; i++) big["k" + i] = i;
        const pat = { patternProperties: { "^k": {} },
                      unevaluatedProperties: false };
        let threw = null;
        try { Schema.validate(pat, big); } catch (e) { threw = e; }
        assert(threw instanceof RangeError &&
               /evaluated properties/.test(String(threw)),
               "up: over-cap tracking fails closed with a RangeError");
    }
}

if (fails) {
    print("test_schema_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_schema_audit failed");
}
print("test_schema_audit: " + n + " assertions, 0 failures");
