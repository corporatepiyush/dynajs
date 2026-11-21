// covers: dyna:time.parseDuration
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_duration");
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

__kitS.section("bare 0", () => {
    assertEq(parseDuration("300ms"), 300000000, "300ms");
    assertEq(parseDuration("1.5h"), 5400000000000, "1.5h");
    assertEq(parseDuration("2h45m"), 9900000000000, "2h45m");
    assertEq(parseDuration("-1m30s"), -90000000000, "-1m30s");
    assertEq(parseDuration("0"), 0, "bare 0");
    assertEq(parseDuration("0s"), 0, "0s");
    assertEq(parseDuration("+0"), 0, "+0");
    assertEq(parseDuration("-0"), 0, "-0 has no sign");
    assertEq(parseDuration("1h30m0s"), 5400000000000, "1h30m0s");
});

__kitS.section("1µs (U+00B5 micro sign)", () => {
    assertEq(parseDuration("1us"), 1000, "1us");
    assertEq(parseDuration("1µs"), 1000, "1µs (U+00B5 micro sign)");
    assertEq(parseDuration("1μs"), 1000, "1μs (U+03BC Greek mu)");
});

__kitS.section("roundtrip", () => {
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
});

__kitS.section("1h", () => {
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
});

__kitS.section("durationString coerces a string to a number", () => {
    assertEq(durationString("1000"), "1µs", "durationString coerces a string to a number");
    assertEq(parseDuration(new String("300ms")), 300000000, "parseDuration coerces a String object");
    let threw = false;
    try { parseDuration(300); } catch (e) { threw = (e instanceof SyntaxError); }
    assert(threw, "parseDuration(300) stringifies to '300' (no unit) and throws");
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
