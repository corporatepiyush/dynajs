// covers: dyna:time.DateParser
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date_parser_strict");
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

__kitS.section("DateParser with {now} parses relative words", () => {
    const anchor = Date.UTC(2026, 0, 15, 12, 0, 0) / 1000;
    const p = new DateParser("en-US", { now: anchor });
    const got = p.parse("tomorrow");
    ok(got !== null, "DateParser with {now} parses relative words");
    const p2 = new DateParser("en-US");
    ok(p2.parse("tomorrow") !== null, "DateParser without opts unchanged");
});
__kitS.end();

throws(() => new DateParser("en-US", { nwo: 1234 }), /unknown option "nwo" \(valid: now\)/,
       "DateParser typo'd `nwo` refuses by name");

throws(() => new DateParser("en-US", { now: 1234, zone: "utc" }), /unknown option "zone"/,
       "DateParser second unknown key refuses");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_time_strict: " + fail + " failures");
