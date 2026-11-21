// covers: dyna:schema.Schema.registerFormat
// dyna:schema referencing and option surface, case by case (the JSON Schema Test Suite run in
// test_schema_conformance.js is the bulk oracle; these pin the API behaviour and the refusals by name):
// $id base-URI changes with RFC 3986 dot-segment resolution, $anchor, $dynamicRef dynamic scope, the resources
// and baseUri options, the embedded 2020-12 meta-schemas, the three format modes, $vocabulary from a custom
// meta-schema, refs into unknown keywords, the legacy "dependencies" keyword, and every new refusal.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_register_format_refs");
import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const valid = (schema, data, opts) => Schema.compile(schema, opts).validate(data).valid;
const throwsT = (f, re, m) => { try { f(); ok(false, m + " did not throw"); } catch (e) { ok(e instanceof TypeError && re.test(e.message), m + " -- " + e.name + ": " + e.message); } };

__kitS.section("not an email", () => {
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
});
__kitS.end();
print("test_schema_refs: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
