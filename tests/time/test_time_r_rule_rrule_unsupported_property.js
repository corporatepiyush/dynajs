// covers: dyna:time.RRule
// RRule.fromString's "unsupported property" error formatted the property name
// AFTER freeing the string it points into: a heap use-after-free read, found by
// the dyna:time input sweep under ASan (tests/time/test_time_inputs.js). The
// san stage runs this suite; outside a sanitizer the message is the oracle.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_rrule_unsupported_property");
import { RRule } from "dyna:time";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

__kitS.section("SyntaxError: dyna:time: unsupported property", () => {
for (const name of ["XFOO", "X".repeat(100), "dtstart-ish", "été"]) {
    const m = err(() => RRule.fromString("FREQ=DAILY;COUNT=2\n" + name + ":1"));
    eq(m, "SyntaxError: dyna:time: unsupported property (" + name + ")", "the message names the property exactly (" + name.slice(0, 12) + ")");
}
});

__kitS.section("SyntaxError: dyna:time: invalid DTSTART in RRULE string", () => {
eq(err(() => RRule.fromString("DTSTART:nope\nRRULE:FREQ=DAILY")), "SyntaxError: dyna:time: invalid DTSTART in RRULE string", "a bad DTSTART (control)");
});

__kitS.section("a supported string parses (control)", () => {
ok(RRule.fromString("FREQ=DAILY;COUNT=2") instanceof RRule, "a supported string parses (control)");
});
__kitS.end();

print("test_rrule_unsupported_property: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
