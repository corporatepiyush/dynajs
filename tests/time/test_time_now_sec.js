// covers: dyna:time.nowSec
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_now_sec");
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

__kitS.section("nowSec() is a number", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
