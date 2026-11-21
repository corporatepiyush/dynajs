// covers: dyna:time.parseTime
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_time_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("HH:MM:SS", () => {
eq(parseTime("07:05:09").toString(), "07:05:09", "HH:MM:SS");
});

__kitS.section("HH:MM:SS.mmm", () => {
eq(parseTime("07:05:09.120").toString(), "07:05:09.120", "HH:MM:SS.mmm");
});

threw(() => parseTime("7:05"), /HH:MM/, "an unpadded hour");
threw(() => parseTime("07:05:09.12"), /HH:MM/, "two-digit milliseconds");
threw(() => parseTime("25:00"), /out of range/, "hour 25 parses but is refused");
threw(() => parseTime("07:05 "), /HH:MM/, "trailing space");

__kitS.section("a time-only duration", () => {
eq(new Duration({ hours: 1, minutes: 2, seconds: 3, milliseconds: 4 }).toString(),
   "PT1H2M3.004S", "a time-only duration");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
