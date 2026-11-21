// covers: dyna:time.date
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date");
import {
    Nanosecond, Microsecond, Millisecond, Second, Minute, Hour,
    durationString, parseDuration,
    parseDurationMs, parseDurationSecs, durationMs, durationSecs,
    now, nowSec, nowUnixNano, nowNanos, nowMillis, monotonicNano,
    formatRFC3339, formatUnix, parseRFC3339,
    date, fromUnix,
    Duration, PlainDate, PlainDateTime, parseDate, dateFromEpochDay,
} from "dyna:time";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    if (actual !== expected)
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected " + expected + ")");
}
function durEq(a, b) {
    return BigInt(a) === BigInt(b);
}

__kitS.section("epoch via date()", () => {
    assertEq(date(1970, 1, 1, 0, 0, 0), 0, "epoch via date()");
    let f = fromUnix(0);
    assertEq(f.year, 1970, "epoch year");
    assertEq(f.month, 1, "epoch month");
    assertEq(f.day, 1, "epoch day");
    assertEq(f.hour, 0, "epoch hour");
    assertEq(f.min, 0, "epoch min");
    assertEq(f.sec, 0, "epoch sec");
    assertEq(f.weekday, 4, "epoch weekday: Thursday (well-known fact)");
    assertEq(f.yday, 1, "epoch yday");

    assertEq(date(2001, 9, 9, 1, 46, 40), 1000000000, "known vector via date()");
    f = fromUnix(1000000000);
    assertEq(f.year, 2001, "1e9 year");
    assertEq(f.month, 9, "1e9 month");
    assertEq(f.day, 9, "1e9 day");
    assertEq(f.hour, 1, "1e9 hour");
    assertEq(f.min, 46, "1e9 min");
    assertEq(f.sec, 40, "1e9 sec");
    assertEq(f.weekday, 0, "1e9 weekday: Sunday");
    assertEq(f.yday, 252, "1e9 yday (31+28+31+30+31+30+31+31+9)");

    const leapSec = date(2000, 2, 29, 12, 0, 0);
    f = fromUnix(leapSec);
    assertEq(f.year, 2000, "leap day year");
    assertEq(f.month, 2, "leap day month");
    assertEq(f.day, 29, "leap day day");
    assertEq(f.yday, 60, "leap day yday (31 + 29)");
    f = fromUnix(leapSec + 43200);
    assertEq(f.month, 3, "day after leap day is March");
    assertEq(f.day, 1, "day after leap day is the 1st");

    f = fromUnix(-1);
    assertEq(f.year, 1969, "pre-epoch year");
    assertEq(f.month, 12, "pre-epoch month");
    assertEq(f.day, 31, "pre-epoch day");
    assertEq(f.hour, 23, "pre-epoch hour");
    assertEq(f.min, 59, "pre-epoch min");
    assertEq(f.sec, 59, "pre-epoch sec");
    assertEq(f.weekday, 3, "pre-epoch weekday: Wednesday");
    assertEq(f.yday, 365, "pre-epoch yday (1969 not a leap year)");
    assertEq(date(1969, 12, 31, 23, 59, 59), -1, "pre-epoch via date()");

    f = fromUnix(date(1900, 2, 28, 0, 0, 0) + DYN_DAY());
    function DYN_DAY() { return 86400; }
    assertEq(f.month, 3, "1900 Feb has only 28 days (non-leap century)");
    assertEq(f.day, 1, "day after 1900-02-28 is March 1st");
});

__kitS.section("block L77", () => {
    const samples = [
        [1, 1, 1, 0, 0, 0], [1969, 1, 1, 0, 0, 0], [1970, 1, 1, 0, 0, 0],
        [1999, 12, 31, 23, 59, 59], [2000, 1, 1, 0, 0, 0],
        [2000, 2, 29, 0, 0, 0], [2004, 2, 29, 0, 0, 0],
        [2023, 2, 28, 0, 0, 0], [2024, 2, 29, 12, 30, 15],
        [2038, 1, 19, 3, 14, 7], [2100, 2, 28, 0, 0, 0],
        [2400, 2, 29, 0, 0, 0], [3000, 1, 1, 0, 0, 0],
        [500, 6, 15, 8, 0, 0],
    ];
    for (const [y, mo, d, h, mi, s] of samples) {
        const sec = date(y, mo, d, h, mi, s);
        const f = fromUnix(sec);
        assertEq(f.year, y, `roundtrip year for ${y}-${mo}-${d}`);
        assertEq(f.month, mo, `roundtrip month for ${y}-${mo}-${d}`);
        assertEq(f.day, d, `roundtrip day for ${y}-${mo}-${d}`);
        assertEq(f.hour, h, `roundtrip hour for ${y}-${mo}-${d}`);
        assertEq(f.min, mi, `roundtrip min for ${y}-${mo}-${d}`);
        assertEq(f.sec, s, `roundtrip sec for ${y}-${mo}-${d}`);
    }
});

__kitS.section("month 13 -> next Jan", () => {
    assertEq(date(2020, 13, 1, 0, 0, 0), date(2021, 1, 1, 0, 0, 0), "month 13 -> next Jan");
    assertEq(date(2020, 0, 1, 0, 0, 0), date(2019, 12, 1, 0, 0, 0), "month 0 -> prior Dec");
    assertEq(date(2020, 25, 1, 0, 0, 0), date(2022, 1, 1, 0, 0, 0), "month 25 -> +2 years, Jan");
});

__kitS.section("year vs Date @", () => {
    function checkAgainstJsDate(sec) {
        const jd = new Date(sec * 1000);
        const f = fromUnix(sec);
        assertEq(f.year, jd.getUTCFullYear(), "year vs Date @ " + sec);
        assertEq(f.month, jd.getUTCMonth() + 1, "month vs Date @ " + sec);
        assertEq(f.day, jd.getUTCDate(), "day vs Date @ " + sec);
        assertEq(f.hour, jd.getUTCHours(), "hour vs Date @ " + sec);
        assertEq(f.min, jd.getUTCMinutes(), "min vs Date @ " + sec);
        assertEq(f.sec, jd.getUTCSeconds(), "sec vs Date @ " + sec);
        assertEq(f.weekday, jd.getUTCDay(), "weekday vs Date @ " + sec);
        assertEq(date(f.year, f.month, f.day, f.hour, f.min, f.sec), sec,
            "date() reconstructs the same sec @ " + sec);
    }

    {
        const startSec = date(2020, 1, 1, 0, 0, 0);
        const endSec = date(2025, 1, 1, 0, 0, 0);
        for (let sec = startSec; sec < endSec; sec += 86400)
            checkAgainstJsDate(sec);
    }
    {
        const startSec = date(1899, 12, 20, 0, 0, 0);
        const endSec = date(1900, 3, 10, 0, 0, 0);
        for (let sec = startSec; sec < endSec; sec += 86400)
            checkAgainstJsDate(sec);
    }
    {
        const startSec = date(1999, 12, 20, 0, 0, 0);
        const endSec = date(2000, 3, 10, 0, 0, 0);
        for (let sec = startSec; sec < endSec; sec += 86400)
            checkAgainstJsDate(sec);
    }
    {
        const startSec = date(100, 1, 1, 0, 0, 0);
        const endSec = date(3000, 1, 1, 0, 0, 0);
        const stride = 86400 * 97 + 3661;
        for (let sec = startSec; sec < endSec; sec += stride)
            checkAgainstJsDate(sec);
    }
});

__kitS.section("date(3e11,1,1) throws RangeError (int64 overflow guard)", () => {
    let threw = false;
    try { date(3e11, 1, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "date(3e11,1,1) throws RangeError (int64 overflow guard)");
    threw = false;
    try { date(-3e11, 1, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "date(-3e11,1,1) throws RangeError too");

    threw = false;
    try { date(275760, 13, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "date(275760,13,1): month carry past the cap throws");

    const fMax = fromUnix(date(275760, 12, 31));
    assertEq(fMax.year, 275760, "date(275760,12,31) works (upper boundary year)");
    assertEq(fMax.month, 12, "upper boundary month");
    assertEq(fMax.day, 31, "upper boundary day");
    const fMin = fromUnix(date(-271821, 1, 1));
    assertEq(fMin.year, -271821, "date(-271821,1,1) works (lower boundary year)");
    threw = false;
    try { date(275761, 1, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "date(275761,1,1) throws (one past the cap)");
    threw = false;
    try { date(-271822, 1, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "date(-271822,1,1) throws (one past the cap)");

    assertEq(parseRFC3339("275760-01-01T00:00:00Z").sec, date(275760, 1, 1),
             "6-digit year at the cap parses");
    assertEq(parseRFC3339("-271821-01-01T00:00:00Z").sec, date(-271821, 1, 1),
             "signed 6-digit year at the cap parses");
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
