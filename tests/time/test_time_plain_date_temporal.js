// covers: dyna:time.PlainDate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_plain_date_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("2000 is a leap year", () => {
eq(new PlainDate(2000, 2, 29).toString(), "2000-02-29", "2000 is a leap year");
});

__kitS.section("February 2024 has 29 days", () => {
eq(new PlainDate(2024, 2, 1).daysInMonth, 29, "February 2024 has 29 days");
});

__kitS.section("1900 is NOT a leap year", () => {
eq(new PlainDate(1900, 2, 1).daysInMonth, 28, "1900 is NOT a leap year");
});

__kitS.section("nor is 2100", () => {
eq(new PlainDate(2100, 2, 1).daysInMonth, 28, "nor is 2100");
});

threw(() => new PlainDate(1900, 2, 29), /has no day 29/, "1900-02-29");
threw(() => new PlainDate(2023, 2, 29), /has no day 29/, "2023-02-29");
threw(() => new PlainDate(2023, 4, 31), /has no day 31/, "31 April");
threw(() => new PlainDate(2023, 13, 1), /month 13/, "month 13");
threw(() => new PlainDate(2023, 0, 1), /month 0/, "month 0");
threw(() => new PlainDate(2023, 1, 0), /has no day 0/, "day 0");
threw(() => new PlainDate(2023), /all three are required/, "too few arguments");

__kitS.section("31 Jan + 1 month clamps to 29 Feb in a leap year", () => {
eq(new PlainDate(2024, 1, 31).add(new Duration({ months: 1 })).toString(),
   "2024-02-29", "31 Jan + 1 month clamps to 29 Feb in a leap year");
});

__kitS.section("and to 28 Feb otherwise", () => {
eq(new PlainDate(2023, 1, 31).add(new Duration({ months: 1 })).toString(),
   "2023-02-28", "and to 28 Feb otherwise");
});

__kitS.section("29 Feb + 1 year clamps", () => {
eq(new PlainDate(2024, 2, 29).add(new Duration({ years: 1 })).toString(),
   "2025-02-28", "29 Feb + 1 year clamps");
});

__kitS.section("subtract clamps the same way", () => {
eq(new PlainDate(2024, 3, 31).subtract(new Duration({ months: 1 })).toString(),
   "2024-02-29", "subtract clamps the same way");
});

__kitS.section("segment L41", () => {
eq(new PlainDate(1, 1, 1).epochDay, -719162, "0001-01-01");
});

__kitS.section("a year outside 0..9999 is written expanded, never ambiguously", () => {
eq(new PlainDate(-1, 1, 1).toString(), "-000001-01-01",
   "a year outside 0..9999 is written expanded, never ambiguously");
});

__kitS.section("and above 9999", () => {
eq(new PlainDate(12345, 6, 7).toString(), "+012345-06-07", "and above 9999");
});

__kitS.section("year-min Jan 1 epoch day", () => {
eq(new PlainDate(-271821, 1, 1).epochDay, -100000109, "year-min Jan 1 epoch day");
});

__kitS.section("year-max Dec 31 epoch day", () => {
eq(new PlainDate(275760, 12, 31).epochDay, 100000109, "year-max Dec 31 epoch day");
});

__kitS.section("until spans months then days", () => {
eq(new PlainDate(2024, 1, 31).until(new PlainDate(2024, 3, 30)).toString(),
   "P1M30D", "until spans months then days");
});

__kitS.section("a zero span", () => {
eq(new PlainDate(2024, 1, 1).until(new PlainDate(2024, 1, 1)).toString(),
   "P0D", "a zero span");
});

__kitS.section("a backwards span is negative", () => {
eq(new PlainDate(2024, 3, 1).until(new PlainDate(2024, 1, 1)).sign, -1,
   "a backwards span is negative");
});

__kitS.section("before", () => {
eq(new PlainDate(2024, 1, 1).compare(new PlainDate(2024, 1, 2)), -1, "before");
});

__kitS.section("after", () => {
eq(new PlainDate(2024, 1, 2).compare(new PlainDate(2024, 1, 1)), 1, "after");
});

__kitS.section("equal", () => {
eq(new PlainDate(2024, 1, 1).compare(new PlainDate(2024, 1, 1)), 0, "equal");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
