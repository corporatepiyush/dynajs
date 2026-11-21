// covers: dyna:time.dateFromEpochDay
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date_from_epoch_day_temporal");
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

__kitS.section("day", () => {
    let mismatched = 0, checked = 0;
    for (let ed = -3653; ed < 14610; ed += 1) {
        const p = dateFromEpochDay(ed);
        const j = new Date(ed * 86400000);
        checked++;
        if (p.year !== j.getUTCFullYear() || p.month !== j.getUTCMonth() + 1
            || p.day !== j.getUTCDate()
            || p.dayOfWeek !== (j.getUTCDay() === 0 ? 7 : j.getUTCDay())
            || p.epochDay !== ed) {
            if (++mismatched < 4)
                print("FAIL day " + ed + ": " + p.toString() + " vs "
                      + j.toISOString().slice(0, 10));
        }
    }
    n++; if (mismatched) bad++;
    eq(checked, 18263, "every day in the range was checked");
    eq(mismatched, 0, "no calendar mismatch against Date");
});

eq(dateFromEpochDay(-719162).toString(), "0001-01-01", "and back");

eq(dateFromEpochDay(-100000109).toString(), "-271821-01-01", "epoch-day domain reaches year-min");

eq(dateFromEpochDay(100000109).toString(), "+275760-12-31", "and year-max");

threw(() => dateFromEpochDay(-100000110), /out of range/, "one day past year-min refuses");
threw(() => dateFromEpochDay(100000110), /out of range/, "one day past year-max refuses");
threw(() => new PlainDate(-271821, 1, 1).subtract(new Duration({ days: 1 })),
      /year -271822 is outside|leaves the supported/, "subtracting past year-min still refuses");
threw(() => new PlainDate(275760, 12, 31).add(new Duration({ days: 1 })),
      /year 275761 is outside|leaves the supported/, "adding past year-max still refuses");
__kitS.section("round trip @", () => {
    for (const ed of [-100000109, -100000108, -100000000, 100000000, 100000108, 100000109])
        eq(dateFromEpochDay(ed).epochDay, ed, "round trip @ " + ed);
});
__kitS.end();

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
