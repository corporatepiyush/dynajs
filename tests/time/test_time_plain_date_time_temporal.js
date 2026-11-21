// covers: dyna:time.PlainDateTime
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_plain_date_time_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("one millisecond before midnight crosses back over the leap day", () => {
eq(new PlainDateTime(2024, 3, 1, 0, 0).subtract(new Duration({ milliseconds: 1 }))
     .toString(), "2024-02-29T23:59:59.999",
   "one millisecond before midnight crosses back over the leap day");
});

__kitS.section("the carry lands on 29 February in a leap year", () => {
eq(new PlainDateTime(2024, 2, 28, 23, 0).add(new Duration({ hours: 2 })).toString(),
   "2024-02-29T01:00:00", "the carry lands on 29 February in a leap year");
});

__kitS.section("and skips it otherwise", () => {
eq(new PlainDateTime(2023, 2, 28, 23, 0).add(new Duration({ hours: 2 })).toString(),
   "2023-03-01T01:00:00", "and skips it otherwise");
});

__kitS.section("month arithmetic still clamps, time untouched", () => {
eq(new PlainDateTime(2024, 1, 31, 12, 0).add(new Duration({ months: 1 })).toString(),
   "2024-02-29T12:00:00", "month arithmetic still clamps, time untouched");
});

__kitS.section("back across a year boundary", () => {
eq(new PlainDateTime(2024, 1, 1, 0, 0).subtract(new Duration({ days: 1 })).toString(),
   "2023-12-31T00:00:00", "back across a year boundary");
});

__kitS.section("toPlainDate", () => {
eq(new PlainDateTime(2024, 3, 1, 23, 30).toPlainDate().toString(), "2024-03-01",
   "toPlainDate");
});

__kitS.section("toPlainTime", () => {
eq(new PlainDateTime(2024, 3, 1, 23, 30).toPlainTime().toString(), "23:30:00",
   "toPlainTime");
});

__kitS.section("dayOfWeek delegates", () => {
eq(new PlainDateTime(2024, 3, 1, 23, 30).dayOfWeek, 5, "dayOfWeek delegates");
});

__kitS.section("compare orders by instant, not by field", () => {
eq(new PlainDateTime(2024, 3, 1, 1, 0).compare(new PlainDateTime(2024, 3, 1, 2, 0)),
   -1, "compare orders by instant, not by field");
});

__kitS.section("and across a date boundary", () => {
eq(new PlainDateTime(2024, 3, 1, 23, 0).compare(new PlainDateTime(2024, 3, 2, 1, 0)),
   -1, "and across a date boundary");
});

__kitS.section("the time defaults to midnight", () => {
eq(new PlainDateTime(2024, 1, 1).toString(), "2024-01-01T00:00:00",
   "the time defaults to midnight");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
