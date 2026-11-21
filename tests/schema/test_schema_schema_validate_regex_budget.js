// covers: dyna:schema.Schema.validate
// tests/schema/test_schema_schema_validate_regex_budget.js -- schema "pattern" and
// "patternProperties" come from schemas, often untrusted, so dyna:schema runs
// them under its own budget of 100M backtracking steps (plus 1000 per subject
// character) even though RegExp in general has no step limit (B10). Past the
// budget validate() throws a catchable RangeError instead of hanging. The
// budget is per call: a plain RegExp in the same program still runs to its
// answer.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_schema_schema_validate_regex_budget");
import { Schema } from "dyna:schema";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

function expectBudget(schema, instance, label) {
    const t0 = cpuNow();
    try {
        Schema.validate(schema, instance);
        check(false, label + ": validate returned instead of throwing");
    } catch (e) {
        check(e instanceof RangeError && /pattern execution timed out/.test(e.message), label + ": RangeError, got " + e);
    }
    const ms = cpuNow() - t0;
    check(ms < 30000, label + ": stopped by the budget, not by finishing (" + Math.round(ms) + " ms)");
}

const evil = "a".repeat(40) + "b";

expectBudget({ pattern: "(a+)+$" }, evil, "pattern");

expectBudget({ patternProperties: { "(a+)+$": { type: "string" } } }, { [evil]: "x" }, "patternProperties");

__kitS.section("an ordinary pattern still validates", () => {
check(Schema.validate({ pattern: "^a+b$" }, evil).valid === true, "an ordinary pattern still validates");
});

__kitS.section("a small catastrophic case finishes under the budget", () => {
check(Schema.validate({ pattern: "(a+)+$" }, "a".repeat(12) + "b").valid === false, "a small catastrophic case finishes under the budget");
});

const t0 = cpuNow();

__kitS.section("a plain RegExp past the schema budget still returns its answer", () => {
check(/(a+)+$/.test("a".repeat(25) + "b") === false, "a plain RegExp past the schema budget still returns its answer");
print("  plain RegExp 2^25 paths took " + Math.round(cpuNow() - t0) + " ms");
});
__kitS.end();

print("test_schema_regex_budget: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_schema_regex_budget: " + failures + " failures");
