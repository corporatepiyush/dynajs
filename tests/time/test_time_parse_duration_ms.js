// covers: dyna:time.parseDurationMs
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_duration_ms");
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

__kitS.section("300ms -> 300ms as a number", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
