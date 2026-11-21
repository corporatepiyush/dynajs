// covers: dyna:time.Microsecond
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_microsecond");
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

__kitS.section("Microsecond = 1000ns", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
