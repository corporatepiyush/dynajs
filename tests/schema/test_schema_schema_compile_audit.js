// covers: dyna:schema.Schema.compile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_compile_audit");
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

no({ contains: { const: 1 }, maxContains: 1, minContains: 2 }, [1, 1],
   "maxContains < minContains can never pass");
no({ contains: { const: 1 }, minContains: 1 }, [], "contains+minContains 1 on []");
throws(() => Schema.compile({ contains: {} , minContains: -1 }),
       "negative minContains is refused at compile");

throws(() => Schema.compile({ contains: {}, maxContains: 1.5 }),
       "fractional maxContains is refused at compile");

__kitS.section("compile from a JSON string", () => {
    assert(Schema.compile('{"type":"integer"}').validate(1).valid === true,
           "compile from a JSON string");
});

assert(Schema.compile({ $dynamicRef: "#/$defs/x", $defs: { x: { type: "integer" } } }).validate("a").valid === false,
       "$dynamicRef is enforced (applicator, never ignored)");

assert(Schema.compile({ $dynamicAnchor: "n", $dynamicRef: "#n" }).validate(1).valid === false,
       "a $dynamicRef that resolves to its own schema is a reported cycle, not a hang");

throws(() => Schema.compile({ $defs: { a: { $ref: "#/$defs/b" }, b: { $ref: "#/$defs/a" } }, $ref: "#/$defs/a" }),
       "pure ref cycle refused at compile");

throws(() => Schema.compile({ $ref: "#" }), "self ref cycle refused at compile");

throws(() => Schema.compile(false ? null : deepAllOf(257)), "schema depth cap 256");

function deepAllOf(d) {
    let s = { type: "integer" };
    for (let i = 0; i < d; i++) s = { allOf: [s] };
    return s;
}

throws(() => Schema.compile({ enum: list(1 << 20 + 1) }), "enum over 2^20 refused");

throws(() => Schema.compile({ type: list(1 << 20 + 1) }), "type array over 2^20 refused");

throws(() => Schema.compile({ pattern: "a".repeat(64 * 1024 + 1) }),
       "pattern source cap 64KiB");

function list(k) { const a = []; for (let i = 0; i < k; i++) a.push("x"); return a; }

__kitS.section("instance nesting past 512 throws (fail-closed, not invalid)", () => {
    const s = { $defs: { a: { items: { $ref: "#/$defs/a" } } }, $ref: "#/$defs/a" };
    let inst = [], cur = inst;
    for (let i = 0; i < 513; i++) { const nx = []; cur.push(nx); cur = nx; }
    throws(() => Schema.compile(s) && Schema.validate(s, inst),
           "instance nesting past 512 throws (fail-closed, not invalid)");
});
__kitS.end();

no({ multipleOf: 0.123456789 }, 1e308, "1e308 vs fractional multiple (overflow path)");
no({ minimum: 0 }, NaN, "NaN satisfies no numeric bound");
throws(() => Schema.compile({ minimum: "5" }), "minimum rejects a string");

throws(() => Schema.compile({ minimum: true }), "minimum rejects a boolean");

throws(() => Schema.compile({ minLength: "3" }), "minLength rejects a string");

throws(() => Schema.compile({ maxItems: null }), "maxItems rejects null");

if (fails) {
    print("test_schema_audit: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_schema_audit failed");
}
print("test_schema_audit: " + n + " assertions, 0 failures");
