// covers: dyna:time.durationString
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_string");
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

__kitS.section("0 -> 0s", () => {
    assertEq(durationString(0), "0s", "0 -> 0s");
    assertEq(durationString(5400000000000), "1h30m0s", "1h30m0s");
    assertEq(durationString(1500000000), "1.5s", "1.5s");
    assertEq(durationString(300000000), "300ms", "300ms");
    assertEq(durationString(-45000000000), "-45s", "-45s");
});

__kitS.section("999ns (just under 1us)", () => {
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
});

__kitS.section("BigInt input", () => {
    assertEq(durationString(90n * BigInt(Second)), "1m30s", "BigInt input");
    assertEq(durationString(0n), "0s", "BigInt zero");
    assertEq(durationString(-5400000000000n), "-1h30m0s", "BigInt negative");
    const intMin = -9223372036854775808n;
    const s = durationString(intMin);
    assert(s.startsWith("-2562047h47m16."), "INT64_MIN formats as ~2562047h: " + s);
    assert(durEq(parseDuration(s), intMin), "INT64_MIN round-trips: " + s);
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
