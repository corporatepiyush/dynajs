// covers: dyna:time.PlainDate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_plain_date");
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

__kitS.section("PlainDate year 0", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
