// covers: dyna:time.now
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_now");
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

__kitS.section("now().sec is a number", () => {
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
});
__kitS.end();

print("test_time: all tests passed (" + n + " assertions)");
