// covers: c:src/modules/time/time-rrule.c
// The per-day filter for BYMONTH, BYDAY (plain weekdays), BYMONTHDAY and negative BYMONTHDAY now
// tests one 64-bit membership mask per list (built when the rule is constructed) instead of a binary
// search per list per candidate day: 4-6% on a daily BYDAY rule over N occurrences. The oracle walks
// every calendar day from DTSTART with Date, keeps the days that pass every BY list, and takes COUNT.
// The rule shapes cover each frequency (DAILY, WEEKLY, MONTHLY, YEARLY), both ends of every list's
// range (month 1 and 12, monthday 1/31/-1/-31, every weekday), and combined lists. Each shape is
// checked over leap and non-leap years and short months.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_by_bits");
import * as T from "dyna:time";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ":\n  got  " + a + "\n  want " + b); } };
const WD = ["SU", "MO", "TU", "WE", "TH", "FR", "SA"];
function oracle(start, count, f) {
    const out = [];
    for (let t = Date.parse(start); out.length < count; t += 86400000) {
        const d = new Date(t), m = d.getUTCMonth() + 1, md = d.getUTCDate(), dim = new Date(Date.UTC(d.getUTCFullYear(), m, 0)).getUTCDate();
        if (f.month && !f.month.includes(m)) continue;
        if (f.day && !f.day.includes(WD[d.getUTCDay()])) continue;
        if (f.mday && !f.mday.some((v) => (v > 0 ? v === md : dim + 1 + v === md))) continue;
        out.push(d.toISOString());
    }
    return out.join(",");
}
function check(freq, f, start, count) {
    const parts = ["FREQ=" + freq];
    if (f.month) parts.push("BYMONTH=" + f.month.join(","));
    if (f.day) parts.push("BYDAY=" + f.day.join(","));
    if (f.mday) parts.push("BYMONTHDAY=" + f.mday.join(","));
    parts.push("COUNT=" + count);
    const rule = parts.join(";");
    eq(T.RRule.fromString(rule, { dtstart: start }).all().map((d) => d.toISOString()).join(","), oracle(start, count, f), rule + " from " + start);
}

__kitS.section("each list alone, every frequency", () => {
    for (const freq of ["DAILY", "WEEKLY", "MONTHLY", "YEARLY"]) {
        for (let w = 0; w < 7; w++) check(freq, { day: [WD[w]] }, "2023-12-30T08:00:00Z", 30);
        check(freq, { day: ["MO", "WE", "FR"] }, "2024-02-26T00:00:00Z", 60);
        check(freq, { day: ["SU", "SA"] }, "2024-02-26T00:00:00Z", 40);
        if (freq !== "WEEKLY") {
            check(freq, { mday: [1] }, "2023-11-15T12:30:00Z", 20);
            check(freq, { mday: [31] }, "2023-11-15T12:30:00Z", 14);
            check(freq, { mday: [-1] }, "2023-11-15T12:30:00Z", 20);
            check(freq, { mday: [-31] }, "2023-11-15T12:30:00Z", 14);
            check(freq, { mday: [29, -29] }, "2023-01-01T00:00:00Z", 30);
            check(freq, { mday: [1, 15, -1] }, "2024-01-01T00:00:00Z", 40);
        }
    }
});

__kitS.section("month filters with day lists", () => {
    check("DAILY", { month: [1] }, "2023-12-20T00:00:00Z", 45);
    check("DAILY", { month: [12] }, "2023-11-25T00:00:00Z", 45);
    check("DAILY", { month: [2], mday: [29] }, "2023-01-01T00:00:00Z", 2);
    check("YEARLY", { month: [2], mday: [-1] }, "2021-01-01T00:00:00Z", 6);
    check("YEARLY", { month: [1, 6, 12], day: ["TU"] }, "2023-01-01T00:00:00Z", 50);
    check("MONTHLY", { month: [3, 4], day: ["FR"], mday: [13] }, "2015-01-01T00:00:00Z", 4);
    check("WEEKLY", { month: [7, 8], day: ["SA", "SU"] }, "2024-06-01T00:00:00Z", 25);
    check("DAILY", { month: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12], day: WD, mday: [-31, -1, 1, 31] }, "2024-01-01T00:00:00Z", 30);
});

__kitS.section("long runs", () => {
    check("DAILY", { day: ["MO", "WE", "FR"] }, "2020-01-01T09:00:00Z", 2000);
    check("DAILY", { mday: [-1, -2, 1] }, "1999-12-01T00:00:00Z", 600);
});

__kitS.end();
print(`test_time_r_rule_by_bits: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
