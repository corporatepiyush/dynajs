// covers: dyna:time.PlainDate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_plain_date_twins");
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

__kitS.section("a.add(a.until(b)) === b for", () => {
    const rand = prng(0x51ce);
    let mixedSeen = 0;
    for (let i = 0; i < 1000; i++) {
        const ya = 1900 + Math.floor(rand() * 201);
        const ma = 1 + Math.floor(rand() * 12);
        const da = 1 + Math.floor(rand() * daysInMonth(ya, ma));
        const ha = Math.floor(rand() * 24);
        const mia = Math.floor(rand() * 60);
        const sa = Math.floor(rand() * 60);
        const msa = Math.floor(rand() * 1000);
        const yb = 1900 + Math.floor(rand() * 201);
        const mb = 1 + Math.floor(rand() * 12);
        const db = 1 + Math.floor(rand() * daysInMonth(yb, mb));
        const hb = Math.floor(rand() * 24);
        const mib = Math.floor(rand() * 60);
        const sb = Math.floor(rand() * 60);
        const msb = Math.floor(rand() * 1000);
        const a = new PlainDateTime(ya, ma, da, ha, mia, sa, msa);
        const b = new PlainDateTime(yb, mb, db, hb, mib, sb, msb);
        const u = a.until(b);
        assertEq(String(a.add(u)), String(b),
            "a.add(a.until(b)) === b for " + a + " -> " + b);
        assertEq(u.months, a.toPlainDate().until(b.toPlainDate()).months,
            "until months parity with PlainDate for " + a + " -> " + b);
        assertEq(u.blank, String(a) === String(b), "until blank iff equal");
        if (!u.blank) {
            let threw = false;
            try { String(u); } catch (e) { threw = e instanceof RangeError; }
            if (threw) {
                mixedSeen++;
                assert((u.months !== 0 || u.years !== 0) && u.days === 0,
                    "toString thrower is the documented mixed-sign shape");
                assertEq(String(a.add(u)), String(b),
                    "mixed-sign value still additive");
            }
        }
    }
    assert(mixedSeen > 0, "corpus exercised the mixed-sign corner (" +
        mixedSeen + " cases)");

    const t = (y, mo, d, h, mi) => new PlainDateTime(y, mo, d, h, mi);
    assertEq(String(t(2026, 8, 17, 23, 30).until(t(2026, 8, 18, 1, 30))), "PT2H",
        "same-month forward folds to time components");
    assertEq(String(t(2026, 8, 18, 1, 30).until(t(2026, 8, 17, 23, 30))), "-PT2H",
        "same-month reverse negates exactly");
    assertEq(String(t(2026, 8, 17, 10, 0).until(t(2026, 8, 17, 10, 0))), "P0D",
        "until self is P0D");
    {
        const a = t(2024, 1, 10, 15, 0), b = t(2024, 1, 11, 9, 0);
        assertEq(String(a.until(b)), "PT18H", "0.75 day is 18h, not 1 day - 6h");
        assertEq(String(a.add(a.until(b))), String(b), "and it adds back");
    }
    {
        const a = t(2024, 1, 10, 15, 0), b = t(2024, 2, 10, 9, 0);
        const u = a.until(b);
        assertEq(u.months, 1, "mixed-sign case keeps the month");
        assertEq(u.days, 0, "mixed-sign case has no whole days");
        assertEq(u.sign, 1, "sign getter takes the leading (months) component");
        assertThrows(() => String(u), RangeError,
            "mixed-sign until has no ISO representation");
        assertEq(String(a.add(u)), String(b), "mixed-sign value still additive");
        const g = b.until(a);
        assertEq(g.months, -1, "reverse keeps -1 month");
        assertEq(g.days, 0, "reverse has no whole days");
        assertEq(g.sign, -1, "reverse sign from the months component");
        assertThrows(() => String(g), RangeError,
            "reverse is mixed-sign too and refuses toString");
        assertEq(String(b.add(g)), String(a), "reverse value additive from b");
    }
    {
        const a = t(2024, 1, 31, 23, 30), b = t(2024, 3, 1, 0, 30);
        assertEq(String(a.until(b)), "P1MT1H", "clamp fold: 1 month + 1 hour");
        assertEq(String(a.add(a.until(b))), String(b), "clamp fold adds back");
        assertEq(String(new PlainDate(2024, 1, 31).until(new PlainDate(2024, 3, 1))),
                 "P1M1D", "PlainDate parity on the same span (dates only)");
    }
    {
        const fwd = new PlainDate(2024, 1, 31).until(new PlainDate(2024, 3, 30));
        const rev = new PlainDate(2024, 3, 30).until(new PlainDate(2024, 1, 31));
        assertEq(String(fwd), "P1M30D", "forward clamp fold");
        assertEq(String(rev), "-P1M29D", "reverse clamp folds a different day");
    }
    {
        const a = new PlainDateTime(2024, 1, 31, 5, 0);
        const b = new PlainDateTime(2024, 3, 30, 7, 0);
        assertEq(String(a.add(a.until(b))), String(b), "forward additive");
        assertEq(String(b.add(b.until(a))), String(a), "reverse additive");
    }
    {
        const rand2 = prng(0x9e37);
        for (let i = 0; i < 300; i++) {
            const y = 1900 + Math.floor(rand2() * 201);
            const mo = 1 + Math.floor(rand2() * 12);
            const a = new PlainDateTime(y, mo, 1 + Math.floor(rand2() * daysInMonth(y, mo)),
                Math.floor(rand2() * 24), Math.floor(rand2() * 60),
                Math.floor(rand2() * 60), Math.floor(rand2() * 1000));
            const b = new PlainDateTime(y, mo, 1 + Math.floor(rand2() * daysInMonth(y, mo)),
                Math.floor(rand2() * 24), Math.floor(rand2() * 60),
                Math.floor(rand2() * 60), Math.floor(rand2() * 1000));
            const f = a.until(b), g = b.until(a);
            if (f.sign === 0) {
                assertEq(String(f), "P0D", "blank until string");
                assertEq(String(g), "P0D", "reverse blank until string");
            } else if (f.sign === 1) {
                assertEq(String(g), "-" + String(f),
                    "same-month negation for " + a + " -> " + b);
            } else {
                assertEq(String(g), String(f).slice(1),
                    "same-month negation for " + a + " -> " + b);
            }
        }
    }
    assertThrows(() => new PlainTime(1, 0).add(new Duration({ months: 1 })),
        RangeError, "PlainTime still refuses month durations");
});
__kitS.end();

print("test_time_twins: all tests passed (" + n + " assertions)");
