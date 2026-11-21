// covers: dyna:time.toPlainDateTime
import {
    parseDuration, parseDurationMs, parseDurationSecs,
    durationMs, durationSecs, durationString,
    PlainDate, PlainTime, PlainDateTime, toPlainDateTime, Duration,
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
function assertThrows(fn, ctor, msg) {
    n++;
    let threw = false, was = null;
    try { fn(); } catch (e) { threw = true; was = e; }
    if (!threw) throw new Error("assertion failed (no throw): " + msg);
    if (!(was instanceof ctor))
        throw new Error("assertion failed (wrong error " + was + "): " + msg);
}

function prng(seed) {
    let s = seed | 0;
    return function () {
        s ^= s << 13; s |= 0;
        s ^= s >>> 17;
        s ^= s << 5; s |= 0;
        return (s >>> 0) / 4294967296;
    };
}

function isoMsOracle(ms) {
    const neg = ms < 0;
    const a = neg ? -ms : ms;
    const h = Math.floor(a / 3600000);
    const mi = Math.floor(a / 60000) % 60;
    const s = Math.floor(a / 1000) % 60;
    const mss = a % 1000;
    let out = (neg ? "-" : "") + "P";
    if (h || mi || s || mss) {
        out += "T";
        if (h) out += h + "H";
        if (mi) out += mi + "M";
        if (s || mss) {
            out += mss ? (s + "." + String(mss).padStart(3, "0") + "S")
                       : (s + "S");
        }
    }
    if (out === "P" || out === "-P") out += "0D";
    return out;
}

function daysInMonth(y, m) {
    const D = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if (m === 2 && ((y % 4 === 0 && y % 100 !== 0) || y % 400 === 0)) return 29;
    return D[m - 1];
}

{
    const rand = prng(0xb5f7);
    for (let i = 0; i < 1000; i++) {
        const y = 1 + Math.floor(rand() * 9998);
        const mo = 1 + Math.floor(rand() * 12);
        const d = 1 + Math.floor(rand() * daysInMonth(y, mo));
        const h = Math.floor(rand() * 24);
        const mi = Math.floor(rand() * 60);
        const s = Math.floor(rand() * 60);
        const ms = Math.floor(rand() * 1000);
        const dt = new PlainDateTime(y, mo, d, h, mi, s, ms);
        const joined = toPlainDateTime(dt.toPlainDate(), dt.toPlainTime());
        assertEq(String(joined), String(dt),
            "join(split) identity for " + y + "-" + mo + "-" + d);
        assertEq(joined.year, dt.year, "join year");
        assertEq(joined.millisecond, dt.millisecond, "join ms");
        assertEq(String(toPlainDateTime(new PlainDate(y, mo, d),
                                        new PlainTime(h, mi, s, ms))),
                 String(new PlainDateTime(y, mo, d, h, mi, s, ms)),
                 "joiner equals ctor");
    }
    const edge = toPlainDateTime(new PlainDate(2026, 1, 1),
                                 new PlainTime(23, 59, 59, 999));
    assertEq(String(edge.add(durationMs(1))), "2026-01-02T00:00:00",
        "last ms of day + 1ms carries into the next date");
    const pd = new PlainDate(2026, 1, 1);
    const pt = new PlainTime(1, 2);
    assertThrows(() => toPlainDateTime(pd), TypeError, "missing time argument");
    assertThrows(() => toPlainDateTime(), TypeError, "missing both arguments");
    assertThrows(() => toPlainDateTime(42, pt), TypeError, "date not a PlainDate");
    assertThrows(() => toPlainDateTime(pd, 42), TypeError, "time not a PlainTime");
    assertThrows(() => toPlainDateTime(pt, pd), TypeError, "swapped arguments");
}

print("test_time_twins: all tests passed (" + n + " assertions)");
