// covers: dyna:schema.Schema.compile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_compile");
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

throws(() => Schema.compile('[1,2]'), "an array is not a schema");

throws(() => Schema.compile("not json"), "unparsable schema string throws");

__kitS.section("a pattern source over 64 KiB is refused at compile time", () => {
    const big = "a".repeat(64 * 1024 + 1);
    throws(() => Schema.compile({ type: "string", pattern: big }),
           "a pattern source over 64 KiB is refused at compile time");
    throws(() => Schema.compile({ type: "object",
                                  patternProperties: { [big]: {} } }),
           "patternProperties go through the same cap");
    ok(Schema.compile({ type: "string", pattern: "a+" }).validate("aaa").valid,
       "a normal pattern still compiles");
});
__kitS.end();

if (fails) {
    print("test_schema: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_schema failed");
}
print("test_schema: " + n + " assertions, 0 failures");
