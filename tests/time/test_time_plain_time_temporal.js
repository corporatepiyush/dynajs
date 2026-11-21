// covers: dyna:time.PlainTime
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_plain_time_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("adding across midnight wraps", () => {
eq(new PlainTime(23, 30).add(new Duration({ hours: 2 })).toString(), "01:30:00",
   "adding across midnight wraps");
});

__kitS.section("and subtracting wraps back", () => {
eq(new PlainTime(0, 30).subtract(new Duration({ hours: 2 })).toString(), "22:30:00",
   "and subtracting wraps back");
});

__kitS.section("one millisecond before midnight", () => {
eq(new PlainTime(0, 0).subtract(new Duration({ milliseconds: 1 })).toString(),
   "23:59:59.999", "one millisecond before midnight");
});

__kitS.section("a whole day returns the same time of day", () => {
eq(new PlainTime(12, 0).add(new Duration({ days: 1 })).toString(), "12:00:00",
   "a whole day returns the same time of day");
});

__kitS.section("time compare before", () => {
eq(new PlainTime(1, 0).compare(new PlainTime(2, 0)), -1, "time compare before");
});

__kitS.section("time compare equal", () => {
eq(new PlainTime(2, 0).compare(new PlainTime(2, 0)), 0, "time compare equal");
});

threw(() => new PlainTime(24, 0), /hour 24/, "24:00");
threw(() => new PlainTime(0, 60), /minute 60/, "minute 60");
threw(() => new PlainTime(0, 0, 60), /second 60/, "second 60 (no leap seconds)");
threw(() => new PlainTime(0, 0, 0, 1000), /millisecond 1000/, "millisecond 1000");
threw(() => new PlainTime(-1), /hour -1/, "a negative hour");
threw(() => new PlainTime(1, 0).add(new Duration({ months: 1 })), /months has no meaning/,
      "a month added to a time of day");

__kitS.section("HH:MM", () => {
eq(parseTime("07:05").toString(), "07:05:00", "HH:MM");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
