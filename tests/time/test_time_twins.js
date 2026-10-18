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
}

{
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

{
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
}

print("test_time_twins: all tests passed (" + n + " assertions)");
