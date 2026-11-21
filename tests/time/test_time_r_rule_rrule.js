// covers: dyna:time.RRule
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_rrule");
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
function throwsMsg(fn, re, m) {
    n++;
    try { fn(); } catch (e) {
        if (!(e instanceof RangeError))
            throw new Error((m || "wrong error type") + ": " + e);
        if (!re.test(String(e.message)))
            throw new Error((m || "wrong message") + ": " + e.message);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
function E(y, m, d, h = 0, mi = 0, s = 0) {
    return Date.UTC(y, m - 1, d, h, mi, s) / 1000;
}
function secs(dates) { return dates.map(d => d.getTime() / 1000); }

__kitS.section("last workday of each month", () => {
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

    const all = r.all();
    assert(all.every(d => d instanceof Date), "all() returns Date objects");
    assert(all[0].getTime() === E(2026,1,30) * 1000, "Date epoch ms is exact");
});

__kitS.section("biweekly Tuesdays", () => {
    eq(secs(new RRule({ freq: "WEEKLY", interval: 2, dtstart: E(2026,1,5),
                        byweekday: ["TU"], count: 5 }).all()),
       [E(2026,1,6), E(2026,1,20), E(2026,2,3), E(2026,2,17), E(2026,3,3)],
       "biweekly Tuesdays");

    eq(secs(new RRule({ freq: "MONTHLY", dtstart: E(2026,1,31), count: 6 }).all()),
       [E(2026,1,31), E(2026,3,31), E(2026,5,31), E(2026,7,31),
        E(2026,8,31), E(2026,10,31)],
       "MONTHLY day-31 skips shorter months");

    eq(secs(new RRule({ freq: "WEEKLY", dtstart: E(2025,12,31), count: 4 }).all()),
       [E(2025,12,31), E(2026,1,7), E(2026,1,14), E(2026,1,21)],
       "WEEKLY default keeps dtstart's weekday");

    eq(secs(new RRule({ freq: "YEARLY", dtstart: E(2024,2,29), count: 3 }).all()),
       [E(2024,2,29), E(2028,2,29), E(2032,2,29)],
       "YEARLY February 29 jumps to leap years only");

    eq(secs(new RRule({ freq: "DAILY", dtstart: E(2026,1,1),
                        byweekday: ["MO", "TH"], count: 6 }).all()),
       [E(2026,1,1), E(2026,1,5), E(2026,1,8), E(2026,1,12),
        E(2026,1,15), E(2026,1,19)],
       "DAILY Mondays and Thursdays");

    eq(secs(new RRule({ freq: "MONTHLY", interval: 3, bymonthday: [1],
                        dtstart: E(2026,1,1), count: 5 }).all()),
       [E(2026,1,1), E(2026,4,1), E(2026,7,1), E(2026,10,1), E(2027,1,1)],
       "quarterly on the 1st");

    eq(secs(new RRule({ freq: "WEEKLY", interval: 4, byweekday: ["TU"],
                        dtstart: E(2026,1,5), count: 5 }).all()),
       [E(2026,1,6), E(2026,2,3), E(2026,3,3), E(2026,3,31), E(2026,4,28)],
       "every fourth Tuesday");

    eq(secs(new RRule({ freq: "YEARLY", bymonth: [2, 5], bymonthday: [15],
                        bysetpos: [1], dtstart: E(2020,1,1), count: 6 }).all()),
       [E(2020,2,15), E(2021,2,15), E(2022,2,15), E(2023,2,15),
        E(2024,2,15), E(2025,2,15)],
       "February 15 every year");
});

__kitS.section("BYWEEKNO=1 Monday crosses into the previous year", () => {
    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], byweekday: ["MO"],
                        dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2027,1,4), E(2028,1,3), E(2029,1,1), E(2029,12,31)],
       "BYWEEKNO=1 Monday crosses into the previous year");

    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], byweekday: ["MO"],
                        bysetpos: [-1], dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2027,1,4), E(2028,1,3), E(2029,12,31), E(2030,12,30)],
       "BYSETPOS=-1 x BYWEEKNO=1 x BYDAY=MO");

    eq(secs(new RRule({ freq: "YEARLY", byweekno: [1], bysetpos: [-1],
                        dtstart: E(2026,1,1), count: 4 }).all()),
       [E(2026,1,4), E(2027,1,10), E(2028,1,9), E(2029,12,31)],
       "BYSETPOS=-1 x BYWEEKNO=1");

    eq(secs(new RRule({ freq: "YEARLY", byweekno: [53],
                        dtstart: E(2026,1,1), count: 6 }).all()),
       [E(2026,12,28), E(2026,12,29), E(2026,12,30), E(2026,12,31),
        E(2027,1,1), E(2027,1,2)],
       "week 53 in 2026 crosses into 2027");
});

__kitS.section("between inclusive", () => {
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

    throws(() => r.all(), RangeError, "infinite rule all() throws RangeError");
    eq(secs(r.all(3)), [E(2026,1,1), E(2026,1,2), E(2026,1,3)], "infinite rule all(limit)");
});

__kitS.section("toString contains FREQ=MONTHLY", () => {
    const str = "RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1;COUNT=6";
    const parsed = RRule.fromString(str, { dtstart: E(2026,1,1) });
    assert(parsed.toString().indexOf("FREQ=MONTHLY") >= 0, "toString contains FREQ=MONTHLY");
    assert(parsed.toString().indexOf("BYSETPOS=-1") >= 0, "toString contains BYSETPOS=-1");
    eq(secs(parsed.all()), [E(2026,1,30), E(2026,2,27), E(2026,3,31),
                            E(2026,4,30), E(2026,5,29), E(2026,6,30)],
       "fromString produces identical occurrences");
});

__kitS.section("SECONDLY between over huge window throws", () => {
    const sec = new RRule({ freq: "SECONDLY", dtstart: E(2020,1,1) });
    throwsMsg(() => sec.between(E(2020,1,1), E(2022,1,1)),
              /budget exhausted \(1000000 periods\)/, "SECONDLY between over huge window throws");
    // R3-5/P4: when the 2^28 work cap fires first, the message names it.
    {
        const pos = [];
        for (let i = 0; i < 800; i++) pos.push(366);
        const heavy = new RRule({ freq: "DAILY", dtstart: E(2026,1,1), bysetpos: pos });
        throwsMsg(() => heavy.all(2000000),
                  /budget exhausted \(the 2\^28 day\/position work cap\)/,
                  "DAILY+800 BYSETPOS exhaustion names the work cap");
        throwsMsg(() => heavy.next(E(2026,1,2)),
                  /budget exhausted \(the 2\^28 day\/position work cap\)/,
                  "work-cap next() message names the work cap");
    }

    const ten = sec.between(E(2020,1,1), E(2020,1,1,0,0,10), true);
    eq(ten.length, 11, "SECONDLY in-budget between returns all 11 seconds");

    const cnt = new RRule({ freq: "SECONDLY", dtstart: E(2020,1,1), count: 1000000000 });
    throwsMsg(() => cnt.next(E(2021,3,1)),
              /budget exhausted/, "counted SECONDLY next far outside budget throws");
    throwsMsg(() => cnt.prev(E(2021,3,1)),
              /budget exhausted/, "counted SECONDLY prev far outside budget throws");

    throws(() => cnt.all(2000000), RangeError,
           "all(limit beyond results cap) throws RangeError");
    eq(secs(cnt.all(3)), [E(2020,1,1), E(2020,1,1,0,0,1), E(2020,1,1,0,0,2)],
       "all(3) under budget still exact");

    const d = new RRule({ freq: "DAILY", dtstart: E(2020,1,1), count: 5 });
    eq(secs(d.between(E(2020,1,1), E(2020,1,10), true)),
       [E(2020,1,1), E(2020,1,2), E(2020,1,3), E(2020,1,4), E(2020,1,5)],
       "counted daily between complete");
    eq(secs(d.between(E(2020,1,1), E(2020,1,10))),
       [E(2020,1,2), E(2020,1,3), E(2020,1,4), E(2020,1,5)],
       "counted daily between exclusive drops start");
    assert(d.next(E(2020,1,3)).getTime() / 1000 === E(2020,1,4),
           "counted daily next found");
    assert(d.next(E(2020,1,10)) === null, "counted daily next past end is null");
    eq(d.all().length, 5, "counted daily all complete");
});
__kitS.section("BYDAY ordinals before FREQ=WEEKLY fold to plain weekdays", () => {
    // fromString reads parts in text order, so BYDAY=1MO arrives before the
    // freq is known and is stored as an ordinal; the compile step folds it
    // into BYDAY for sub-monthly freqs. It used to append to an
    // uninitialised count (ASan negative-size memmove, empty BYDAY in
    // release, every day emitted instead of Mon/Wed).
    const a = RRule.fromString("DTSTART:20240101T090000Z\nRRULE:BYDAY=1MO,2WE;FREQ=WEEKLY;COUNT=4");
    const b = RRule.fromString("DTSTART:20240101T090000Z\nRRULE:FREQ=WEEKLY;BYDAY=1MO,2WE;COUNT=4");
    eq(a.toString(), "RRULE:FREQ=WEEKLY;COUNT=4;BYDAY=MO,WE", "ordinal BYDAY first folds");
    eq(a.all().map(d => d.toISOString()), ["2024-01-01T09:00:00.000Z", "2024-01-03T09:00:00.000Z",
        "2024-01-08T09:00:00.000Z", "2024-01-10T09:00:00.000Z"], "Mon/Wed weekly");
    eq(a.all().map(d => d.getTime()), b.all().map(d => d.getTime()), "same as FREQ first");
    const c = RRule.fromString("DTSTART:20240101T090000Z\nRRULE:BYDAY=-1FR,MO;FREQ=DAILY;COUNT=3");
    eq(c.all().map(d => d.toISOString().slice(0, 10)), ["2024-01-01", "2024-01-05", "2024-01-08"], "daily keeps plain MO with folded FR");
});
__kitS.end();

print("test_rrule: all tests passed (" + n + " assertions)");
