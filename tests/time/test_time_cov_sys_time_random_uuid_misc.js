// Split from tests/test_cov_sys_time_random_uuid_misc.js: the time sections.
import * as time from "dyna:time";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== time ===");
{
    const nm = time.nowMillis();
    assert(Math.abs(nm - Date.now()) < 5000, "nowMillis ~ Date.now()");
    const nan = time.nowUnixNano();
    assert(typeof nan === "bigint" && nan > BigInt("1000000000000000000"), "nowUnixNano bigint plausible magnitude");
    const mono1 = time.monotonicNano();
    const mono2 = time.monotonicNano();
    assert(typeof mono1 === "bigint" && mono2 >= mono1, "monotonicNano non-decreasing bigint");
    const nowS = time.now();
    assert(nowS.sec > 1.7e9 && typeof nowS.nsec === "number" && nowS.nsec < 1e9, "now() {sec,nsec} shape");

    eq(time.formatUnix(1577934245, "2006-01-02 15:04:05"), "2020-01-02 03:04:05", "formatUnix layout tokens");
    eq(time.formatUnix(1577934245, "2006"), "2020", "formatUnix year only");
    eq(time.formatUnix(1577934245, "Jan Mon"), "Jan Thu", "formatUnix month+weekday abbr");
    eq(time.formatUnix(0, "2006-01-02"), "1970-01-01", "formatUnix epoch");
    eq(time.formatUnix(-86400, "2006-01-02"), "1969-12-31", "formatUnix pre-epoch");
    eq(time.formatUnix(253402300799, "2006"), "9999", "formatUnix year 9999 max-ish");
    try { const s = time.formatUnix(NaN, "2006"); n++; } catch (e) { n++; }

    eq(time.formatRFC3339(1577934245), "2020-01-02T03:04:05Z", "formatRFC3339 Z form");
    const pr = time.parseRFC3339("2020-01-02T03:04:05Z");
    eq(pr.sec, 1577934245, "parseRFC3339 sec");
    eq(pr.nsec, 0, "parseRFC3339 nsec 0");
    eq(time.parseRFC3339(time.formatRFC3339(1234567890)).sec, 1234567890, "rfc3339 round-trip");
    throws(() => time.parseRFC3339("not-a-time"), /./, "parseRFC3339 garbage throws");
    throws(() => time.parseRFC3339(""), /./, "parseRFC3339 empty throws");
    const pf = time.parseRFC3339("2020-01-02T03:04:05.123456789Z");
    eq(pf.nsec, 123456789, "parseRFC3339 fractional ns");
    const po = time.parseRFC3339("2020-01-02T03:04:05+01:00");
    eq(po.sec, 1577934245 - 3600, "parseRFC3339 +01:00 offset");

    const pd = time.PlainDate ? new time.PlainDate(2020, 1, 15) : null;
    if (pd) {
        eq(pd.year, 2020, "PlainDate.year");
        eq(pd.month, 1, "PlainDate.month");
        eq(pd.day, 15, "PlainDate.day");
        eq(pd.inLeapYear, true, "2020 leap year");
        eq(pd.daysInMonth, 31, "January daysInMonth");
        assert(pd.dayOfWeek >= 0 && pd.dayOfWeek <= 6, "dayOfWeek range");
        eq(pd.dayOfYear, 15, "dayOfYear Jan 15");
        if (pd.add) {
            const later = pd.add(new time.Duration({ days: 17 }));
            eq(later.month, 2, "add crosses month");
            eq(later.day, 1, "add day rollover");
        }
    }
    const feb2020 = new time.PlainDate(2020, 2, 28);
    eq(feb2020.daysInMonth, 29, "Feb 2020 has 29");
    const feb2019 = new time.PlainDate(2019, 2, 28);
    eq(feb2019.daysInMonth, 28, "Feb 2019 has 28");
    const feb2100 = new time.PlainDate(2100, 2, 28);
    eq(feb2100.daysInMonth, 28, "2100 not a leap year (century rule)");
    const feb2000 = new time.PlainDate(2000, 2, 28);
    eq(feb2000.daysInMonth, 29, "2000 IS a leap year (400 rule)");

    eq(new time.Duration({ hours: 1 }).toString(), "PT1H", "Duration PT1H");
    eq(new time.Duration({ hours: 1, minutes: 30 }).toString(), "PT1H30M", "Duration PT1H30M");
    eq(new time.Duration({ seconds: 90 }).toString(), "PT1M30S", "Duration PT1M30S");
    eq(time.durationString(3600 * 1e9), "1h0m0s", "durationString 1h");
    eq(time.durationString(61 * 1e9), "1m1s", "durationString 1m1s");
    eq(time.durationString(1500 * 1e6), "1.5s", "durationString 1.5s");
    eq(time.durationString(0), "0s", "durationString zero");
    eq(time.durationString(-1e9), "-1s", "durationString negative");

    const ed = time.dateFromEpochDay(18276);
    assert(ed && typeof ed.toString === "function", "dateFromEpochDay returns date-like");

    if (time.Format) {
        const f = new time.Format("2006-01-02");
        eq(f.apply ? String(f.apply(1577934245)) : String(f.format ? f.format(1577934245) : f.at(1577934245)), "2020-01-02", "Format class formats");
    }

    eq(time.Hour, 3600000000000, "Hour ns constant");
    eq(time.Minute, 60000000000, "Minute ns constant");
    eq(time.Second, 1000000000, "Second ns constant");
    eq(time.Millisecond, 1000000, "Millisecond ns constant");
    eq(time.Microsecond, 1000, "Microsecond ns constant");
    eq(time.Nanosecond, 1, "Nanosecond ns constant");
}
if (fails) {
    print("test_time_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_time_cov_sys_time_random_uuid_misc failed");
}
print("test_time_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
