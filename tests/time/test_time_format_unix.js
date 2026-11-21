// covers: dyna:time.formatUnix
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_unix");
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

__kitS.section("date tokens", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
