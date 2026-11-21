// covers: dyna:time.parseDate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_date_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("a valid date", () => {
eq(parseDate("2024-02-29").toString(), "2024-02-29", "a valid date");
});

__kitS.section("an expanded year", () => {
eq(parseDate("+012345-06-07").toString(), "+012345-06-07", "an expanded year");
});

__kitS.section("a negative year", () => {
eq(parseDate("-000001-01-01").toString(), "-000001-01-01", "a negative year");
});

threw(() => parseDate("2024-2-9"), /not an ISO 8601/, "unpadded fields");
threw(() => parseDate("2024/02/29"), /not an ISO 8601/, "slashes");
threw(() => parseDate("2024-02-29T00:00"), /not an ISO 8601/, "trailing time");
threw(() => parseDate(" 2024-02-29"), /not an ISO 8601/, "leading space");
threw(() => parseDate("0x10-02-29"), /not an ISO 8601/, "a hex-looking year");
threw(() => parseDate("2023-02-29"), /has no day 29/, "a well-formed impossible date");
threw(() => parseDate(""), /not an ISO 8601/, "empty");

__kitS.section("weeks fold into days and years into months, both exactly", () => {
eq(new Duration({ years: 1, months: 2, weeks: 3, days: 4 }).toString(), "P1Y2M25D",
   "weeks fold into days and years into months, both exactly");
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
