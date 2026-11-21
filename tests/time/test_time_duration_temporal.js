// covers: dyna:time.Duration
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("months first, then days", () => {
    const d = new PlainDate(2024, 1, 30);
    const a = d.add(new Duration({ months: 1, days: 1 }));
    const b = d.add(new Duration({ days: 1 })).add(new Duration({ months: 1 }));
    eq(a.toString(), "2024-03-01", "months first, then days");
    eq(b.toString(), "2024-02-29", "days first gives a different date");
});

eq(new Duration({}).toString(), "P0D", "an empty duration");

eq(new Duration({}).blank, true, "and it is blank");

eq(new Duration({ months: -3 }).toString(), "-P3M", "a negative duration");

eq(new Duration({ months: 14 }).years, 1, "years are months / 12");

threw(() => new Duration(), /options object/, "no argument");
threw(() => new PlainDate(2024, 1, 1).add(42), /Duration/, "adding a non-Duration");

__kitS.section("toString keeps milliseconds", () => {
    const t = new PlainTime(9, 30, 15, 250);
    eq(t.toString(), "09:30:15.250", "toString keeps milliseconds");
    eq(t.hour, 9, "hour"); eq(t.minute, 30, "minute");
    eq(t.second, 15, "second"); eq(t.millisecond, 250, "millisecond");
    eq(t.msSinceMidnight, 34215250, "milliseconds since midnight");
    eq(new PlainTime(9, 30).toString(), "09:30:00",
       "seconds are always printed, so the shape never depends on the value");
});

eq(new Duration({ days: 2, hours: 3 }).toString(), "P2DT3H",
   "days and hours stay on opposite sides of the T");

eq(new Duration({ milliseconds: 0 }).blank, true, "a zero time part is blank");

eq(new Duration({ seconds: -5 }).sign, -1, "a negative time-only duration");

__kitS.section("toString is date T time", () => {
    const dt = new PlainDateTime(2024, 3, 1, 23, 30);
    eq(dt.toString(), "2024-03-01T23:30:00", "toString is date T time");
    eq(dt.add(new Duration({ hours: 2 })).toString(), "2024-03-02T01:30:00",
       "adding across midnight carries into the date");
    eq(new PlainTime(23, 30).add(new Duration({ hours: 2 })).toString(), "01:30:00",
       "the same addition on a PlainTime wraps and loses the day");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
