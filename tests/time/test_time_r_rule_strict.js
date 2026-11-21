// covers: dyna:time.RRule
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_strict");
import { DateParser, RRule } from "dyna:time";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + a + ", want " + b + ")");
const throws = (fn, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof TypeError)) { fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name) + " [" + e.message + "]"); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};

__kitS.section("RRule with {dtstart} still iterates (COUNT=3)", () => {
    const r = RRule.fromString("FREQ=DAILY;COUNT=3",
                           { dtstart: Date.UTC(2026, 0, 1) / 1000 });
    eq(r.all(10).length, 3, "RRule with {dtstart} still iterates (COUNT=3)");
});

throws(() => RRule.fromString("FREQ=DAILY", { dtstrt: 1 }), /unknown option "dtstrt" \(valid: dtstart\)/,
       "RRule typo'd `dtstrt` refuses with the valid set");

throws(() => RRule.fromString("FREQ=DAILY", { dtstart: 1, count: 3 }), /unknown option "count"/,
       "RRule `count` refuses (count lives in the RRULE string, not the bag)");

__kitS.section("RRule without opts unchanged", () => {
    const r = RRule.fromString("FREQ=DAILY;COUNT=2");
    eq(r.all(10).length, 2, "RRule without opts unchanged");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_time_strict: " + fail + " failures");
