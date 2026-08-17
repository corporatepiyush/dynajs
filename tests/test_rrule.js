/* test_rrule.js -- class RRule, RFC 5545 recurrence rules (P1 3.6).
 *
 * Expected values are pinned against vendored python-dateutil rrule output
 * (generated once with tools/gen_rrule_vectors.py -- see that script for the
 * dateutil pin). Every row in this file is a dateutil-verified number, never
 * a value recorded from this engine: the week-numbering (BYWEEKNO), the
 * cross-year week-1/-53 days, the BYSETPOS x BYWEEKNO interaction and the
 * month-end day clamping are the cases a self-consistent implementation
 * gets wrong together.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_rrule.js
 * Prints "test_rrule: all tests passed" on success; throws on failure.
 */
import { RRule } from "dyna:time";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    const g = JSON.stringify(got), w = JSON.stringify(want);
    if (g !== w) throw new Error((m || "mismatch") + ": got " + g + " want " + w);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
/* epoch seconds of a UTC date; every expected value below is UTC. */
function E(y, m, d, h = 0, mi = 0, s = 0) {
    return Date.UTC(y, m - 1, d, h, mi, s) / 1000;
}
function secs(dates) { return dates.map(d => d.getTime() / 1000); }

/* ==================================================================== *
 *  1. The brief's headline rule: the last workday of each month.
 *     BYSETPOS=-1 over a 5-weekday BYDAY set.
 * ==================================================================== */
{
    const r = new RRule({
        freq: "MONTHLY",
        dtstart: E(2026, 1, 1),
        byweekday: ["MO", "TU", "WE", "TH", "FR"],
        bysetpos: [-1],
        count: 6,
    });
    eq(secs(r.all()), [E(2026,1,30), E(2026,2,27), E(2026,3,31),
                        E(2026,4,30), E(2026,5,29), E(2026,6,30)],
       "last workday of each month");

    /* the exact RFC 5545 string from the brief */
    const bare = new RRule({
        freq: "MONTHLY",
        byweekday: ["MO", "TU", "WE", "TH", "FR"],
        bysetpos: [-1],
        dtstart: E(2026, 1, 1),
    });
    eq(bare.toString(), "RRULE:FREQ=MONTHLY;BYSETPOS=-1;BYDAY=MO,TU,WE,TH,FR",
       "toString matches the brief's canonical string");

    const parsed = RRule.fromString(
        "RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1",
        { dtstart: E(2026, 1, 1) });
    eq(secs(parsed.all(6)), [E(2026,1,30), E(2026,2,27), E(2026,3,31),
                             E(2026,4,30), E(2026,5,29), E(2026,6,30)],
       "fromString of the brief string reproduces the rule");

    /* the returned values are real Dates */
    const all = r.all();
    assert(all.every(d => d instanceof Date), "all() returns Date objects");
    assert(all[0].getTime() === E(2026,1,30) * 1000, "Date epoch ms is exact");
}

/* ==================================================================== *
 *  2. Frequencies, intervals, defaults
 * ==================================================================== */
{
    /* WEEKLY;INTERVAL=2;BYDAY=TU -- every second Tuesday */
    eq(secs(new RRule({ freq: "WEEKLY", interval: 2, dtstart: E(2026,1,5),
                        byweekday: ["TU"], count: 5 }).all()),
       [E(2026,1,6), E(2026,1,20), E(2026,2,3), E(2026,2,17), E(2026,3,3)],
       "biweekly Tuesdays");

    /* MONTHLY with no by* repeats the dtstart day (months without it are
     * skipped, exactly as dateutil's bymonthday=dtstart.day does) */
    eq(secs(new RRule({ freq: "MONTHLY", dtstart: E(2026,1,31), count: 6 }).all()),
       [E(2026,1,31), E(2026,3,31), E(2026,5,31), E(2026,7,31),
        E(2026,8,31), E(2026,10,31)],
       "MONTHLY day-31 skips shorter months");

    /* WEEKLY with no by* keeps the dtstart weekday (a Wednesday here) */
    eq(secs(new RRule({ freq: "WEEKLY", dtstart: E(2025,12,31), count: 4 }).all()),
       [E(2025,12,31), E(2026,1,7), E(2026,1,14), E(2026,1,21)],
       "WEEKLY default keeps dtstart's weekday");

    /* YEARLY with no by* repeats the dtstart month/day */
    eq(secs(new RRule({ freq: "YEARLY", dtstart: E(2024,2,29), count: 3 }).all()),
       [E(2024,2,29), E(2028,2,29), E(2032,2,29)],
       "YEARLY February 29 jumps to leap years only");

    /* DAILY with BYDAY=MO,TH */
    eq(secs(new RRule({ freq: "DAILY", dtstart: E(2026,1,1),
                        byweekday: ["MO", "TH"], count: 6 }).all()),
       [E(2026,1,1), E(2026,1,5), E(2026,1,8), E(2026,1,12),
        E(2026,1,15), E(2026,1,19)],
       "DAILY Mondays and Thursdays");

    /* MONTHLY;INTERVAL=3;BYMONTHDAY=1 */
    eq(secs(new RRule({ freq: "MONTHLY", interval: 3, bymonthday: [1],
                        dtstart: E(2026,1,1), count: 5 }).all()),
       [E(2026,1,1), E(2026,4,1), E(2026,7,1), E(2026,10,1), E(2027,1,1)],
       "quarterly on the 1st");

    /* WEEKLY;INTERVAL=4;BYDAY=TU */
    eq(secs(new RRule({ freq: "WEEKLY", interval: 4, byweekday: ["TU"],
                        dtstart: E(2026,1,5), count: 5 }).all()),
       [E(2026,1,6), E(2026,2,3), E(2026,3,3), E(2026,3,31), E(2026,4,28)],
       "every fourth Tuesday");

    /* a YEARLY rule with BYMONTH+BYMONTHDAY skips nothing in between */
    eq(secs(new RRule({ freq: "YEARLY", bymonth: [2, 5], bymonthday: [15],
                        bysetpos: [1], dtstart: E(2020,1,1), count: 6 }).all()),
       [E(2020,2,15), E(2021,2,15), E(2022,2,15), E(2023,2,15),
        E(2024,2,15), E(2025,2,15)],
       "February 15 every year");
}

/* ==================================================================== *
 *  3. BYWEEKNO -- ISO-style week numbering, and its cross-year days
 * ==================================================================== */
{
    /* first Monday of ISO week 1. 2029's week 1 starts in 2029 but the
     * fourth result is 2029-12-31: the Monday of week 1 of 2030, emitted
     * during 2029 because that week starts before 2029 ends. */
    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], byweekday: ["MO"],
                        dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2027,1,4), E(2028,1,3), E(2029,1,1), E(2029,12,31)],
       "BYWEEKNO=1 Monday crosses into the previous year");

    /* BYSETPOS x BYWEEKNO: the LAST Monday of week 1 of each year */
    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], byweekday: ["MO"],
                        bysetpos: [-1], dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2027,1,4), E(2028,1,3), E(2029,12,31), E(2030,12,30)],
       "BYSETPOS=-1 x BYWEEKNO=1 x BYDAY=MO");

    /* the last day of week 1 -- week 1 alone with BYSETPOS=-1 */
    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], bysetpos: [-1],
                        dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2026,1,4), E(2027,1,10), E(2028,1,9), E(2029,12,31)],
       "BYSETPOS=-1 x BYWEEKNO=1");

    /* week 53: only years that have one; the run spans 2026 -> 2027 */
    eq(secs(new RRule({ freq: "YEARLY", byweekno: [53],
                        dtstart: E(2026,1,1), count: 6 }).all()),
       [E(2026,12,28), E(2026,12,29), E(2026,12,30), E(2026,12,31),
        E(2027,1,1), E(2027,1,2)],
       "week 53 in 2026 crosses into 2027");
}

/* ==================================================================== *
 *  4. between(), next(), prev() windows and infinite bounds
 * ==================================================================== */
{
    const r = new RRule({ freq: "DAILY", dtstart: E(2026,1,1) });
    const b = r.between(E(2026,1,10), E(2026,1,15), true);
    eq(secs(b), [E(2026,1,10), E(2026,1,11), E(2026,1,12), E(2026,1,13), E(2026,1,14), E(2026,1,15)],
       "between inclusive");
    const b_ex = r.between(E(2026,1,10), E(2026,1,15), false);
    eq(secs(b_ex), [E(2026,1,11), E(2026,1,12), E(2026,1,13), E(2026,1,14)],
       "between exclusive");

    const nxt = r.next(E(2026,1,10));
    assert(nxt !== null && nxt.getTime() / 1000 === E(2026,1,11), "next occurrence");

    const prv = r.prev(E(2026,1,10));
    assert(prv !== null && prv.getTime() / 1000 === E(2026,1,9), "prev occurrence");

    /* Infinite rule all() must throw without a limit */
    throws(() => r.all(), RangeError, "infinite rule all() throws RangeError");
    eq(secs(r.all(3)), [E(2026,1,1), E(2026,1,2), E(2026,1,3)], "infinite rule all(limit)");
}

/* ==================================================================== *
 *  5. toString() and fromString() round trips
 * ==================================================================== */
{
    const str = "RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1;COUNT=6";
    const parsed = RRule.fromString(str, { dtstart: E(2026,1,1) });
    assert(parsed.toString().indexOf("FREQ=MONTHLY") >= 0, "toString contains FREQ=MONTHLY");
    assert(parsed.toString().indexOf("BYSETPOS=-1") >= 0, "toString contains BYSETPOS=-1");
    eq(secs(parsed.all()), [E(2026,1,30), E(2026,2,27), E(2026,3,31),
                            E(2026,4,30), E(2026,5,29), E(2026,6,30)],
       "fromString produces identical occurrences");
}

print("test_rrule: all tests passed (" + n + " assertions)");
