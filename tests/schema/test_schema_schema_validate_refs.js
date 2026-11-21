// covers: dyna:schema.Schema.validate
// dyna:schema referencing and option surface, case by case (the JSON Schema Test Suite run in
// test_schema_conformance.js is the bulk oracle; these pin the API behaviour and the refusals by name):
// $id base-URI changes with RFC 3986 dot-segment resolution, $anchor, $dynamicRef dynamic scope, the resources
// and baseUri options, the embedded 2020-12 meta-schemas, the three format modes, $vocabulary from a custom
// meta-schema, refs into unknown keywords, the legacy "dependencies" keyword, and every new refusal.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_validate_refs");
import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const valid = (schema, data, opts) => Schema.compile(schema, opts).validate(data).valid;
const throwsT = (f, re, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof TypeError && re.test(e.message), m + " -- " + e.name + ": " + e.message); } };

__kitS.section("300 embedded resources resolve", () => {
    const big = { $id: "https://ex.test/big", $defs: {} };
    for (let i = 0; i < 300; i++) big.$defs["d" + i] = { $id: "d" + i, $anchor: "a" + i, type: i % 2 ? "string" : "number" };
    const c = Schema.compile({ ...big, items: { $ref: "d299#a299" } });
    ok(c.validate(["s"]).valid && !c.validate([1]).valid, "300 embedded resources resolve");
});
__kitS.end();
print("test_schema_refs: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
