// flags: --std
/* test_time_strict.js -- straggler: dyna:time's pre-existing option
 * bags go strict.
 *
 * Bags covered (the phase-1 enumeration for this module):
 *   new DateParser(str, { now })        -- the deterministic-clock anchor
 *   RRule.fromString(str, { dtstart })  -- the rule's start anchor
 * A typo'd key used to be silently dropped: a DateParser typo'd `now` races
 * the real clock, and an RRule typo'd `dtstart` anchors the recurrence to
 * the wrong start -- both look like they worked.
 */
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

/* ---- 1. the honest keys still work ---- */
{
    const anchor = Date.UTC(2026, 0, 15, 12, 0, 0) / 1000;   // 2026-01-15T12:00Z
    const p = new DateParser("en-US", { now: anchor });
    const got = p.parse("tomorrow");
    ok(got !== null, "DateParser with {now} parses relative words");
    const p2 = new DateParser("en-US");
    ok(p2.parse("tomorrow") !== null, "DateParser without opts unchanged");
}
{
    const r = RRule.fromString("FREQ=DAILY;COUNT=3",
                           { dtstart: Date.UTC(2026, 0, 1) / 1000 });
    eq(r.all(10).length, 3, "RRule with {dtstart} still iterates (COUNT=3)");
}

/* ---- 2. the bogus-key matrix ---- */
throws(() => new DateParser("en-US", { nwo: 1234 }), /unknown option "nwo" \(valid: now\)/,
       "DateParser typo'd `nwo` refuses by name");
throws(() => new DateParser("en-US", { now: 1234, zone: "utc" }), /unknown option "zone"/,
       "DateParser second unknown key refuses");
throws(() => RRule.fromString("FREQ=DAILY", { dtstrt: 1 }), /unknown option "dtstrt" \(valid: dtstart\)/,
       "RRule typo'd `dtstrt` refuses with the valid set");
throws(() => RRule.fromString("FREQ=DAILY", { dtstart: 1, count: 3 }), /unknown option "count"/,
       "RRule `count` refuses (count lives in the RRULE string, not the bag)");

/* ---- 3. an opts object is not required ---- */
{
    const r = RRule.fromString("FREQ=DAILY;COUNT=2");
    eq(r.all(10).length, 2, "RRule without opts unchanged");
}

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_time_strict: " + fail + " failures");
