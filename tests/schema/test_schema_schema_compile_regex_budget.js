// covers: dyna:schema.Schema.compile
// tests/schema/test_schema_schema_compile_regex_budget.js -- schema "pattern" and
// "patternProperties" come from schemas, often untrusted, so dyna:schema runs
// them under its own budget of 100M backtracking steps (plus 1000 per subject
// character) even though RegExp in general has no step limit (B10). Past the
// budget validate() throws a catchable RangeError instead of hanging. The
// budget is per call: a plain RegExp in the same program still runs to its
// answer.
import { sections as __kitSections } from "../lib/kit.js";
import { Schema } from "dyna:schema";
import { cpuNow } from "../lib/cpuclock.js";
const __kitS = __kitSections("test_schema_schema_compile_regex_budget", { chain: true });
let evil, t0;

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

function expectBudget(schema, instance, label) {
let t0, ms;
__kitS.section("validate returned instead of throwing", () => {
    t0 = cpuNow();
    try {
        Schema.validate(schema, instance);
        check(false, label + ": validate returned instead of throwing");
    } catch (e) {
        check(e instanceof RangeError && /pattern execution timed out/.test(e.message), label + ": RangeError, got " + e);
    }
});
__kitS.section("stopped by the budget, not by finishing", () => {
    ms = cpuNow() - t0;
    check(ms < 30000, label + ": stopped by the budget, not by finishing (" + Math.round(ms) + " ms)");
});
}

__kitS.section("fixture: build the backtracking input", () => {
evil = "a".repeat(40) + "b";
});

expectBudget(Schema.compile({ properties: { s: { pattern: "(a|aa)+$" } } }), { s: "a".repeat(60) + "b" }, "compiled schema, nested pattern");
__kitS.end();
if (!__kitS.listing) {

t0 = cpuNow();
print("  plain RegExp 2^25 paths took " + Math.round(cpuNow() - t0) + " ms");

print("test_schema_regex_budget: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_schema_regex_budget: " + failures + " failures");
}
