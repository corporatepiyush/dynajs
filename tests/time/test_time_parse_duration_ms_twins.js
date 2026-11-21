// covers: dyna:time.parseDurationMs
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_duration_ms_twins");
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

__kitS.section("ms twin agrees for", () => {
    const magnitudes = [1, 7, 999, 12345, 250000];
    const units = ["ns", "us", "µs", "μs", "ms", "s", "m", "h"];
    const corpus = ["0", "300ms", "-1.5h", "2h45m", "1h30m0s", "200000h",
        "-200000h", "1.5h", "-1m30s", "1500us", "1us", "999ns",
        "0.000000001s", "9223372036s", "-9223372036s",
        "2562047h47m16.854775807s", "-2562047h47m16.854775808s"];
    for (const u of units)
        for (const mag of magnitudes) {
            corpus.push(mag + u);
            corpus.push("-" + mag + u);
            corpus.push("1.5" + u);
            corpus.push("-1.5" + u);
        }
    for (const s of corpus) {
        const ns = parseDuration(s);
        const msTwin = parseDurationMs(s);
        const secsTwin = parseDurationSecs(s);
        assertEq(msTwin, Number(ns) / 1e6, "ms twin agrees for " + s);
        assertEq(secsTwin, Number(ns) / 1e9, "secs twin agrees for " + s);
        assert(typeof msTwin === "number" && typeof secsTwin === "number",
            "twins are numbers for " + s);
        assert(BigInt(parseDuration(durationString(ns))) === BigInt(ns),
            "durationString roundtrip for " + s);
    }
    assertEq(parseDurationMs("200000h"), 720000000000, "200000h ms exact (7.2e11)");
    assertEq(parseDurationSecs("200000h"), 720000000, "200000h secs exact (7.2e8)");
    assert(Number.isFinite(parseDurationMs("2562047h47m16.854775807s")),
        "INT64_MAX ns in ms is finite");
    assert(Number.isFinite(parseDurationSecs("-2562047h47m16.854775808s")),
        "INT64_MIN ns in secs is finite");
    for (const b of ["", "garbage", "-", "+", ".", "5", "1h30", "1H", "1 h",
                     "99999999999999999999h", "9223372037s", "1z", "1.d"]) {
        assertThrows(() => parseDurationMs(b), SyntaxError, "ms twin refuses " + b);
        assertThrows(() => parseDurationSecs(b), SyntaxError, "secs twin refuses " + b);
    }
});
__kitS.end();

print("test_time_twins: all tests passed (" + n + " assertions)");
