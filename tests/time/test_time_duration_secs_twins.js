// covers: dyna:time.durationSecs
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_secs_twins");
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

__kitS.section("ISO shape", () => {
    const msCorpus = [0, 1, 999, 1000, 1500, 59999, 60000, 61000,
        3599999, 3600000, 3661000, 86399999, 86400000,
        -1, -1500, -3600000, -3661000,
        7.2e11  ];
    const rand = prng(0x7b);
    for (let i = 0; i < 200; i++)
        msCorpus.push(Math.floor(rand() * 4e9) - 2e9);
    for (const ms of msCorpus) {
        const d = durationMs(ms);
        assertEq(String(d), isoMsOracle(ms), "durationMs(" + ms + ") ISO shape");
        assertEq(String(d), String(new Duration({ milliseconds: ms })),
            "durationMs(" + ms + ") mirrors the ctor");
    }
    for (const s of [0, 1, 59, 60, 90, 3600, 3661, 86400, -1, -90, 1.9, -1.9]) {
        assertEq(String(durationSecs(s)), String(new Duration({ seconds: s })),
            "durationSecs(" + s + ") mirrors the ctor");
        assertEq(String(durationSecs(s)), isoMsOracle(Math.trunc(s) * 1000),
            "durationSecs(" + s + ") folds via whole seconds");
    }
    assert(durationSecs(NaN).blank, "durationSecs(NaN) coerces like the ctor (0)");
    assertThrows(() => durationSecs(1e16), RangeError, "durationSecs overflow throws");
    assertThrows(() => durationSecs(-1e16), RangeError, "durationSecs -overflow throws");
    assertEq(String(durationSecs(9e15)), isoMsOracle(9e18),
        "durationSecs at the last representable fold works");
});
__kitS.end();

print("test_time_twins: all tests passed (" + n + " assertions)");
