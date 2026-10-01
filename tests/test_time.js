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

{
    assertEq(Nanosecond, 1, "Nanosecond");
    assertEq(Microsecond, 1000, "Microsecond");
    assertEq(Millisecond, 1000000, "Millisecond");
    assertEq(Second, 1000000000, "Second");
    assertEq(Minute, 60000000000, "Minute");
    assertEq(Hour, 3600000000000, "Hour");
    assertEq(Microsecond, 1000 * Nanosecond, "Microsecond = 1000ns");
    assertEq(Millisecond, 1000 * Microsecond, "Millisecond = 1000us");
    assertEq(Second, 1000 * Millisecond, "Second = 1000ms");
    assertEq(Minute, 60 * Second, "Minute = 60s");
    assertEq(Hour, 60 * Minute, "Hour = 60m");
}

{
    assertEq(durationString(0), "0s", "0 -> 0s");
    assertEq(durationString(5400000000000), "1h30m0s", "1h30m0s");
    assertEq(durationString(1500000000), "1.5s", "1.5s");
    assertEq(durationString(300000000), "300ms", "300ms");
    assertEq(durationString(-45000000000), "-45s", "-45s");
}

{
    assertEq(durationString(1), "1ns", "1ns");
    assertEq(durationString(999), "999ns", "999ns (just under 1us)");
    assertEq(durationString(1000), "1µs", "1us boundary -> 1µs");
    assertEq(durationString(1500), "1.5µs", "1.5us");
    assertEq(durationString(999999), "999.999µs", "999.999us (just under 1ms)");
    assertEq(durationString(1000000), "1ms", "1ms boundary");
    assertEq(durationString(1500000), "1.5ms", "1.5ms");
    assertEq(durationString(999999999), "999.999999ms", "999.999999ms (just under 1s)");
    assertEq(durationString(1000000000), "1s", "1s boundary (not sub-second anymore)");
    assertEq(durationString(65000000000), "1m5s", "65s -> 1m5s (NOT zero-padded)");
    assertEq(durationString(3661000000000), "1h1m1s", "3661s -> 1h1m1s (NOT zero-padded)");
    assertEq(durationString(3600000000000), "1h0m0s", "exactly 1 hour -> 1h0m0s");
    assertEq(durationString(60000000000), "1m0s", "exactly 1 minute -> 1m0s");
    assertEq(durationString(-1), "-1ns", "negative ns");
    assertEq(durationString(-1500), "-1.5µs", "negative us");
    assertEq(durationString(-1500000000), "-1.5s", "negative s");
    assertEq(durationString(-5400000000000), "-1h30m0s", "negative h/m/s");
}

{
    assertEq(durationString(90n * BigInt(Second)), "1m30s", "BigInt input");
    assertEq(durationString(0n), "0s", "BigInt zero");
    assertEq(durationString(-5400000000000n), "-1h30m0s", "BigInt negative");
    const intMin = -9223372036854775808n;
    const s = durationString(intMin);
    assert(s.startsWith("-2562047h47m16."), "INT64_MIN formats as ~2562047h: " + s);
    assert(durEq(parseDuration(s), intMin), "INT64_MIN round-trips: " + s);
}

{
    assertEq(parseDuration("300ms"), 300000000, "300ms");
    assertEq(parseDuration("1.5h"), 5400000000000, "1.5h");
    assertEq(parseDuration("2h45m"), 9900000000000, "2h45m");
    assertEq(parseDuration("-1m30s"), -90000000000, "-1m30s");
    assertEq(parseDuration("0"), 0, "bare 0");
    assertEq(parseDuration("0s"), 0, "0s");
    assertEq(parseDuration("+0"), 0, "+0");
    assertEq(parseDuration("-0"), 0, "-0 has no sign");
    assertEq(parseDuration("1h30m0s"), 5400000000000, "1h30m0s");
}

{
    assertEq(parseDuration("1us"), 1000, "1us");
    assertEq(parseDuration("1µs"), 1000, "1µs (U+00B5 micro sign)");
    assertEq(parseDuration("1μs"), 1000, "1μs (U+03BC Greek mu)");
}

{
    const vectors = [
        0, 1, 999, 1000, 1500, 999999, 1000000, 1500000, 999999999,
        1000000000, 1500000000, 59000000000, 60000000000, 65000000000,
        3600000000000, 3661000000000, 5400000000000,
        -1, -1500, -1500000000, -45000000000, -5400000000000,
    ];
    for (const v of vectors) {
        const str = durationString(v);
        const back = parseDuration(str);
        assert(durEq(back, v), "roundtrip " + v + " -> '" + str + "' -> " + back);
    }
    const big = 10000000000000000n;
    const bigStr = durationString(big);
    const bigBack = parseDuration(bigStr);
    assert(typeof bigBack === "bigint", "parseDuration returns bigint above 2^53");
    assert(durEq(bigBack, big), "large-magnitude roundtrip: " + bigStr);
    assert(typeof parseDuration("300ms") === "number", "parseDuration returns number when safe");
}

{
    const bad = [
        "", "garbage", "-", "+", ".", "1.5", "1.", ".5x", "5x", "5",
        "1h30", "h", "1hh", "1.2.3h", "  1h", "1h ", "1H", "1 h",
        "99999999999999999999h", "9223372037s", "1z",
    ];
    for (const b of bad) {
        let threw = false;
        try { parseDuration(b); } catch (e) {
            threw = (e instanceof SyntaxError);
        }
        assert(threw, "parseDuration(" + JSON.stringify(b) + ") must throw SyntaxError");
    }
    assertEq(parseDuration("1.s"), 1000000000, "'1.s' is valid (bare trailing dot ok)");
}

{
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
}

{
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
}

{
    assertEq(date(2020, 13, 1, 0, 0, 0), date(2021, 1, 1, 0, 0, 0), "month 13 -> next Jan");
    assertEq(date(2020, 0, 1, 0, 0, 0), date(2019, 12, 1, 0, 0, 0), "month 0 -> prior Dec");
    assertEq(date(2020, 25, 1, 0, 0, 0), date(2022, 1, 1, 0, 0, 0), "month 25 -> +2 years, Jan");
}

{
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
}

{
    assertEq(formatRFC3339(0), "1970-01-01T00:00:00Z", "epoch RFC3339");
    assertEq(formatRFC3339(1000000000), "2001-09-09T01:46:40Z", "known vector RFC3339");
    assertEq(formatRFC3339(-1), "1969-12-31T23:59:59Z", "pre-epoch RFC3339");
    assertEq(formatRFC3339(1000000000, 123456789), "2001-09-09T01:46:40.123456789Z",
        "RFC3339 with full nanosecond fraction");
    assertEq(formatRFC3339(1000000000, 500000000), "2001-09-09T01:46:40.5Z",
        "RFC3339 fraction trims trailing zeros");
    assertEq(formatRFC3339(1000000000, 0), "2001-09-09T01:46:40Z",
        "RFC3339 nsec=0 has no fraction");

    for (const [sec, nsec] of [[0, 0], [1000000000, 0], [1000000000, 123456789],
                                 [-1, 0], [1700000000, 5000], [1700000000, 999999999]]) {
        const str = formatRFC3339(sec, nsec);
        const back = parseRFC3339(str);
        assertEq(back.sec, sec, "RFC3339 roundtrip sec for " + str);
        assertEq(back.nsec, nsec, "RFC3339 roundtrip nsec for " + str);
    }

    {
        const negYear = date(-500, 3, 15, 6, 0, 0);
        assertEq(formatRFC3339(negYear), "-0500-03-15T06:00:00Z", "negative year formats with a sign");
        assertEq(parseRFC3339(formatRFC3339(negYear)).sec, negYear, "negative year round-trips");

        const wideYear = date(12345, 6, 7, 8, 9, 10);
        assertEq(formatRFC3339(wideYear), "12345-06-07T08:09:10Z", "5-digit year is not truncated");
        assertEq(parseRFC3339(formatRFC3339(wideYear)).sec, wideYear, "5-digit year round-trips");

        assertEq(formatUnix(negYear, "2006-01-02"), "-0500-03-15",
            "formatUnix's '2006' token matches the same signed/wide shape");
        assertEq(formatUnix(wideYear, "2006-01-02"), "12345-06-07",
            "formatUnix's '2006' token is not truncated for a wide year either");
    }

    assertEq(parseRFC3339("2001-09-09T01:46:40Z").sec, 1000000000, "Z offset");
    assertEq(parseRFC3339("2001-09-09T06:46:40+05:00").sec, 1000000000, "+05:00 offset");
    assertEq(parseRFC3339("2001-09-08T20:46:40-05:00").sec, 1000000000, "-05:00 offset");
    assertEq(parseRFC3339("2001-09-09T07:16:40+05:30").sec, 1000000000, "+05:30 offset (non-hour)");
    assertEq(parseRFC3339("2001-09-09T01:46:40.25Z").nsec, 250000000, "2-digit fraction");
    assertEq(parseRFC3339("2001-09-09T01:46:40.123456789123Z").nsec, 123456789,
        "over-precise fraction truncates to 9 digits, still parses");
    assertEq(parseRFC3339("2001-09-09t01:46:40z").sec, 1000000000,
        "lowercase t/z accepted");

    const badRfc = [
        "", "not-a-date", "2001-09-09", "2001-09-09T01:46:40",
        "2001-13-09T01:46:40Z", "2001-09-32T01:46:40Z", "2001-02-30T00:00:00Z",
        "2001-09-09T25:00:00Z", "2001-09-09T01:60:00Z",
        "2001-09-09T01:46:40+0500", "2001-09-09T01:46:40+05",
        "2001-09-09T01:46:40Zgarbage", "2001-09-09X01:46:40Z",
        "999999999999-01-01T00:00:00Z", "1234567-01-01T00:00:00Z",
    ];
    for (const b of badRfc) {
        let threw = false;
        try { parseRFC3339(b); } catch (e) { threw = (e instanceof SyntaxError); }
        assert(threw, "parseRFC3339(" + JSON.stringify(b) + ") must throw SyntaxError");
    }

    assertEq(fromUnix(date(1970, 1, 1)).weekday, 4, "1970-01-01 Thursday (well known)");
    assertEq(fromUnix(date(2000, 1, 1)).weekday, 6, "2000-01-01 Saturday (well known)");
    assertEq(fromUnix(date(2024, 1, 1)).weekday, 1, "2024-01-01 Monday (well known)");
}

{
    const sec = date(2001, 9, 9, 1, 46, 40);
    assertEq(formatUnix(sec, "2006-01-02"), "2001-09-09", "date tokens");
    assertEq(formatUnix(sec, "15:04:05"), "01:46:40", "time tokens");
    assertEq(formatUnix(sec, "2006-01-02T15:04:05"), "2001-09-09T01:46:40",
        "full combined layout");
    assertEq(formatUnix(sec, "Jan 02, 2006"), "Sep 09, 2001", "month abbrev + zero-padded day");
    assertEq(formatUnix(sec, "Jan 2, 2006"), "Sep 2, 2001", "bare '2' is a literal, not a day token");
    assertEq(formatUnix(sec, "Mon Jan 02 2006"), "Sun Sep 09 2001", "weekday abbrev");
    assertEq(formatUnix(sec, "2006"), "2001", "bare year token");
    assertEq(formatUnix(sec, "no tokens here!"), "no tokens here!", "pure literal passthrough");
    assertEq(formatUnix(sec, ""), "", "empty layout");
    assertEq(formatUnix(sec, "café 2006"), "café 2001",
        "multi-byte UTF-8 literal passes through unchanged");

    assertEq(formatUnix(sec, "2006"), "2001", "token 2006");
    assertEq(formatUnix(sec, "01"), "09", "token 01");
    assertEq(formatUnix(sec, "02"), "09", "token 02");
    assertEq(formatUnix(sec, "15"), "01", "token 15");
    assertEq(formatUnix(sec, "04"), "46", "token 04");
    assertEq(formatUnix(sec, "05"), "40", "token 05");
    assertEq(formatUnix(sec, "Jan"), "Sep", "token Jan");
    assertEq(formatUnix(sec, "Mon"), "Sun", "token Mon");

    assertEq(formatUnix(0, "2006-01-02 15:04:05 Mon"), "1970-01-01 00:00:00 Thu", "epoch layout");

    {
        const filler = "x".repeat(5000);
        const layout = filler + "2006" + filler;
        const out = formatUnix(sec, layout);
        assertEq(out.length, filler.length * 2 + 4, "long layout output length");
        assert(out.startsWith(filler) && out.endsWith(filler), "long layout literal preserved");
        assert(out.slice(filler.length, filler.length + 4) === "2001", "long layout token substituted");
    }
}

{
    const w = now();
    assert(typeof w.sec === "number", "now().sec is a number");
    assert(typeof w.nsec === "number", "now().nsec is a number");
    assert(w.nsec >= 0 && w.nsec < 1000000000, "now().nsec in range");
    assert(w.sec > 1577836800, "now() is after 2020-01-01");
    assert(w.sec < 4102444800, "now() is before 2100-01-01");

    const ms = nowMillis();
    assert(typeof ms === "number", "nowMillis() is a number");
    const wMs = w.sec * 1000 + Math.floor(w.nsec / 1000000);
    assert(Math.abs(ms - wMs) < 5000, "now() close to nowMillis(): " + wMs + " vs " + ms);

    const nano = nowUnixNano();
    assert(typeof nano === "bigint", "nowUnixNano() is a bigint");
    const wNano = BigInt(w.sec) * 1000000000n + BigInt(w.nsec);
    const diff = nano > wNano ? nano - wNano : wNano - nano;
    assert(diff < 5000000000n, "nowUnixNano() close to now(): diff=" + diff + "ns");

    const m0 = monotonicNano();
    let busy = 0;
    for (let i = 0; i < 500000; i++) busy += i;
    const m1 = monotonicNano();
    assert(typeof m0 === "bigint" && typeof m1 === "bigint", "monotonicNano() is a bigint");
    assert(m1 >= m0, "monotonicNano() is non-decreasing: " + m0 + " -> " + m1);
    assert(m1 > m0, "monotonicNano() actually advanced across a busy loop");

    let prev = monotonicNano();
    for (let i = 0; i < 1000; i++) {
        const cur = monotonicNano();
        assert(cur >= prev, "monotonicNano() never decreases call-to-call");
        prev = cur;
    }
}

{
    assertEq(durationString("1000"), "1µs", "durationString coerces a string to a number");
    assertEq(parseDuration(new String("300ms")), 300000000, "parseDuration coerces a String object");
    let threw = false;
    try { parseDuration(300); } catch (e) { threw = (e instanceof SyntaxError); }
    assert(threw, "parseDuration(300) stringifies to '300' (no unit) and throws");
}

{
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
}

{
    const y0 = new PlainDate(0, 1, 1);
    assertEq(y0.year, 0, "PlainDate year 0");
    assertEq(y0.epochDay, -719528, "0000-01-01 epochDay is -719528");
    assertEq(y0.toString(), "0000-01-01", "year 0 formats 4-digit");
    assertEq(parseDate("0000-01-01").year, 0, "parseDate accepts year 0");

    const minY = new PlainDate(-271821, 1, 1);
    assertEq(minY.year, -271821, "PlainDate at TP_MIN_YEAR works");
    assertEq(minY.toString(), "-271821-01-01", "TP_MIN_YEAR formats signed 6-digit");
    const maxY = new PlainDate(275760, 12, 31);
    assertEq(maxY.year, 275760, "PlainDate at TP_MAX_YEAR works");
    assertEq(maxY.toString(), "+275760-12-31", "TP_MAX_YEAR formats signed 6-digit");
    for (const bad of [[-271822, 1, 1], [275761, 1, 1]]) {
        let threw = false;
        try { new PlainDate(bad[0], bad[1], bad[2]); } catch (e) { threw = (e instanceof RangeError); }
        assert(threw, "PlainDate(" + bad[0] + ",...) refuses with RangeError");
    }

    assertEq(parseDate("-271821-01-01").year, -271821, "parseDate at TP_MIN_YEAR");
    assertEq(parseDate("275760-12-31").year, 275760, "parseDate at TP_MAX_YEAR");
    for (const bad of ["-271822-01-01", "275761-01-01"]) {
        let threw = false;
        try { parseDate(bad); } catch (e) { threw = (e instanceof RangeError); }
        assert(threw, "parseDate(" + JSON.stringify(bad) + ") refuses with RangeError");
    }

    assertEq(dateFromEpochDay(-100000000).year, -271821,
             "dateFromEpochDay(-1e8) is exactly TP_MIN_YEAR");
    assertEq(dateFromEpochDay(100000000).year, 275760,
             "dateFromEpochDay(+1e8) is exactly TP_MAX_YEAR");

    assertEq(new PlainDateTime(275760, 12, 31, 23, 59, 59).year, 275760,
             "PlainDateTime at TP_MAX_YEAR works");
    let threw = false;
    try { new PlainDateTime(275761, 1, 1); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "PlainDateTime past TP_MAX_YEAR refuses");
}

{
    assertEq(parseDurationMs("300ms"), 300, "300ms -> 300ms as a number");
    assertEq(parseDurationMs("1.5h"), 5400000, "1.5h in ms");
    assertEq(parseDurationSecs("1.5h"), 5400, "1.5h in secs");
    assertEq(parseDurationMs("-1.5h"), -5400000, "negative h in ms");
    assertEq(parseDurationSecs("-90s"), -90, "negative s in secs");
    assertEq(parseDurationMs("2h45m"), 9900000, "compound 2h45m in ms");
    assertEq(parseDurationMs("1ns"), 0.000001, "1ns is 1e-6 ms (sub-ms precision kept)");
    assertEq(parseDurationMs("1500us"), 1.5, "1500us is 1.5ms");
    assertEq(parseDurationSecs("1ms"), 0.001, "1ms is 0.001s");
    assertEq(parseDurationSecs("1us"), 0.000001, "1us is 1e-6 s");
    assertEq(parseDurationMs("0"), 0, "bare 0 in ms");
    assertEq(typeof parseDurationMs("300ms"), "number", "parseDurationMs always a number");

    assertEq(parseDuration("200000h"), 720000000000000000n, "200000h ns is a BigInt");
    assertEq(parseDurationMs("200000h"), 7.2e11, "200000h ms is exactly 7.2e11");
    assertEq(parseDurationSecs("200000h"), 7.2e8, "200000h secs is exactly 7.2e8");

    for (const s of ["300ms", "1.5h", "2h45m", "-1m30s", "1500us", "1us", "999ns"]) {
        assertEq(parseDurationMs(s), Number(parseDuration(s)) / 1e6,
            "parseDurationMs agrees with parseDuration for " + s);
        assertEq(parseDurationSecs(s), Number(parseDuration(s)) / 1e9,
            "parseDurationSecs agrees with parseDuration for " + s);
    }

    assertEq(parseDurationSecs("9223372036s"), 9223372036,
        "9223372036s in secs is exact (< 2^53)");
    assertEq(parseDurationMs("9223372036s"), 9223372036000,
        "9223372036s in ms is exact (< 2^53)");
    let threw = false;
    try { parseDurationMs("9223372037s"); } catch (e) { threw = (e instanceof SyntaxError); }
    assert(threw, "parseDurationMs throws on int64 overflow like parseDuration");

    for (const b of ["", "garbage", "-", "5", "1h30", "1H", "1z", "99999999999999999999h"]) {
        for (const [name, fn] of [["parseDurationMs", parseDurationMs],
                                  ["parseDurationSecs", parseDurationSecs]]) {
            let bad = false;
            try { fn(b); } catch (e) { bad = (e instanceof SyntaxError); }
            assert(bad, name + "(" + JSON.stringify(b) + ") must throw SyntaxError");
        }
    }

    assertEq(String(durationMs(1500)), "PT1.500S", "durationMs(1500) formats");
    assertEq(String(durationSecs(90)), "PT1M30S", "durationSecs(90) formats");
    assertEq(String(durationSecs(-1.9)), "-PT1S",
        "durationSecs truncates fractional seconds like the ctor");
    assertEq(String(durationSecs(1.5)), "PT1S",
        "durationSecs(1.5) is 1s, NOT 1.5s (use durationMs for sub-second)");
    assertEq(String(durationMs(-1500)), "-PT1.500S", "durationMs negative");
    assertEq(String(durationMs(0)), "P0D", "durationMs(0) is blank");
    assert(durationMs(0).blank, "durationMs(0).blank");
    assertEq(durationMs(-5).sign, -1, "durationMs(-5).sign");
    assertEq(String(durationMs(3661000)), String(new Duration({ milliseconds: 3661000 })),
        "durationMs equals new Duration({milliseconds})");
    assertEq(String(durationSecs(3661)), String(new Duration({ seconds: 3661 })),
        "durationSecs equals new Duration({seconds})");
    assertEq(String(durationSecs(60)), "PT1M",
        "durationSecs(60) folds to minutes");
}

{
    const sec = nowSec();
    assert(typeof sec === "number", "nowSec() is a number");
    assert(Number.isInteger(sec), "nowSec() is whole seconds");
    assert(sec > 1577836800, "nowSec() is after 2020-01-01");
    assert(sec < 4102444800, "nowSec() is before 2100-01-01");
    assert(Math.abs(sec - now().sec) < 5, "nowSec() agrees with now().sec");

    const nanos = nowNanos();
    assert(typeof nanos === "bigint", "nowNanos() is a bigint");
    const uNano = nowUnixNano();
    const nDiff = nanos > uNano ? nanos - uNano : uNano - nanos;
    assert(nDiff < 5000000000n, "nowNanos() agrees with nowUnixNano(): " + nDiff + "ns");
    const sDiff = nanos - BigInt(sec) * 1000000000n;
    assert(sDiff > -5000000000n && sDiff < 5000000000n,
        "nowNanos() agrees with nowSec(): " + sDiff + "ns");
}

print("test_time: all tests passed (" + n + " assertions)");
