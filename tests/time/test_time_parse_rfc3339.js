// covers: dyna:time.parseRFC3339
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_rfc3339");
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

__kitS.section("epoch RFC3339", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
