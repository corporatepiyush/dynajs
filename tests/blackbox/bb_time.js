// Black-box contract test for dyna:time, generated from dynajs.d.ts lines 5571-5831. Engine sources not consulted.
// Table-driven: CASES tables are rows of [label, expected, ...inputs] (refusal tables carry an
// error class + pattern) driven through ONE loop whose failure message names the row.
// All fixed-timestamp expectations are hand-derived arithmetic the doc invokes:
//   epoch day of 2026-08-17 = 20682 (20454 days to 2026-01-01: 56*365+14 leap days; +228 doy-1)
//   -> date(2026,8,17) = 20682*86400 = 1786924800; +10:30:00 = 1786962600
//   1700000000 = day 19675 (+80000 s) = 2023-11-14T22:13:20Z; Nov 14 2023 yday 318, Tuesday
//   1970-01-01 is a Thursday (weekday 4 with 0 = Sunday; ISO dayOfWeek 4);
//   2026-08-17 is a Monday (20682 mod 7 = 4 -> Thursday+4; ISO dayOfWeek 1);
//   1969-12-31 is a Wednesday (weekday 3), yday 365.

import {
    Nanosecond, Microsecond, Millisecond, Second, Minute, Hour,
    parseDuration, parseDurationMs, parseDurationSecs, durationString,
    Duration, durationMs, durationSecs,
    now, nowSec, nowUnixNano, nowNanos, nowMillis, monotonicNano,
    formatRFC3339, formatUnix, parseRFC3339, date, fromUnix,
    Format, PlainDate, PlainDateTime, PlainTime, RRule, DateParser,
    parseDate, dateFromEpochDay, parseTime, toPlainDateTime,
} from "dyna:time";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) {
    n++;
    // HARNESS-FIX: rows may return small arrays (e.g. [sec, nsec]); Object.is alone can never
    // pass for two distinct array literals, so compare their JSON text as well.
    const ok = Object.is(actual, expected) ||
        (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)) ||
        (Array.isArray(actual) && Array.isArray(expected) && JSON.stringify(actual) === JSON.stringify(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b))
        throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType))
        throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern)))
        throw new Error("wrong error message |" + e + "|: " + msg);
}
function throwRows(cases, mod) {
    for (const [label, fn, ErrType, pattern] of cases)
        assertThrows(fn, mod + " [" + label + "]", ErrType, pattern);
}
function thunkRows(cases, mod) {
    for (const [label, expected, fn] of cases) assertEq(fn(), expected, mod + " [" + label + "]");
}
const D = (fields) => new Duration(fields);

/* ==================== unit constants table ==================== */

// "Module constants in nanoseconds"
thunkRows([
    ["Nanosecond", 1, () => Nanosecond],
    ["Microsecond", 1000, () => Microsecond],
    ["Millisecond", 1000000, () => Millisecond],
    ["Second", 1000000000, () => Second],
    ["Minute", 60000000000, () => Minute],
    ["Hour", 3600000000000, () => Hour],
], "time.constants");

/* ==================== parseDuration table ==================== */

// Units ns/us/µs/μs/ms/s/m/h (no "d"); NANOSECONDS result; Number below 2^53, BigInt at/above;
// int64 overflow or malformed input -> "invalid duration" SyntaxError.
thunkRows([
    ["300ms", 300000000, () => parseDuration("300ms")],
    ["1s", 1000000000, () => parseDuration("1s")],
    ["1m", 60000000000, () => parseDuration("1m")],
    ["1.5h", 5400000000000, () => parseDuration("1.5h")],
    ["-1.5h", -5400000000000, () => parseDuration("-1.5h")],
    ["2h45m compound", 9900000000000, () => parseDuration("2h45m")],
    ["0", 0, () => parseDuration("0")],
    ["5ns", 5, () => parseDuration("5ns")],
    ["1us ascii", 1000, () => parseDuration("1us")],
    ["1µs micro sign", 1000, () => parseDuration("1µs")],
    ["1μs greek mu", 1000, () => parseDuration("1μs")],
    ["300ms is a Number below 2^53", "number", () => typeof parseDuration("300ms")],
    ["200000h value (doc-pinned bigint)", 720000000000000000n, () => parseDuration("200000h")],
    ["200000h is a BigInt at 2^53 ns", "bigint", () => typeof parseDuration("200000h")],
], "time.parseDuration");
throwRows([
    ["malformed input", () => parseDuration("nonsense"), SyntaxError, /invalid duration/],
    ["d is NOT accepted", () => parseDuration("10d"), SyntaxError, /invalid duration/],
    ["int64 ns overflow (3000000h = 1.08e19 ns)", () => parseDuration("3000000h"), SyntaxError, /invalid duration/],
], "time.parseDuration.refusals");

// parseDurationMs / parseDurationSecs: "correctly-rounded double of ns/1e6 / ns/1e9"
thunkRows([
    ["300ms", 300, () => parseDurationMs("300ms")],
    ["200000h exact double (doc-pinned 7.2e11)", 720000000000, () => parseDurationMs("200000h")],
    ["1.5h", 5400000, () => parseDurationMs("1.5h")],
    ["1s", 1000, () => parseDurationMs("1s")],
], "time.parseDurationMs");
thunkRows([
    ["1.5h", 5400, () => parseDurationSecs("1.5h")],
    ["300ms is exactly the double 0.3", 0.3, () => parseDurationSecs("300ms")],
    ["1s", 1, () => parseDurationSecs("1s")],
], "time.parseDurationSecs");

/* ==================== durationString table ==================== */

// "The inverse of parseDuration; 0 is '0s' ('2h45m' round-trips as '2h45m0s')" (d.ts);
// "largest unit first, fractions trimmed of trailing zeros"; "below 1 s a single unit by magnitude".
thunkRows([
    ["zero (doc-pinned)", "0s", () => durationString(0)],
    ["2h45m round trip (doc-pinned)", "2h45m0s", () => durationString(parseDuration("2h45m"))],
    ["1.5h largest unit first", "1h30m0s", () => durationString(5400000000000)],
    ["1.5s fraction", "1.5s", () => durationString(1500000000)],
    ["300ms single sub-second unit", "300ms", () => durationString(300000000)],
], "time.durationString");
// inverse property rows: parseDuration(durationString(x)) === x
thunkRows(["300ms", "1.5h", "2h45m", "-1.5h", "90s"].map((s) =>
    ["round trip " + s, true, () => parseDuration(durationString(parseDuration(s))) === parseDuration(s)]),
    "time.durationString.inverse");

/* ==================== Duration class table ==================== */

{
    const d = D({ hours: 1, minutes: 30 });
    const m = D({ months: 14, days: 3 });
    const w = D({ weeks: 1, days: 2 });
    thunkRows([
        ["PT1H30M (doc-pinned)", "PT1H30M", () => String(d)],
        ["sign of a positive duration", 1, () => d.sign],
        ["not blank", false, () => d.blank],
        ["14 months fold to 1y2m (doc-pinned)", 1, () => m.years],
        ["months remainder", 2, () => m.months],
        ["days stay", 3, () => m.days],
        ["weeks fold into days (documented)", 9, () => w.days],
        ["P1M2DT5S (doc-pinned shape)", "P1M2DT5S", () => String(D({ months: 1, days: 2, seconds: 5 }))],
        // TEST-FIX: the docs pin only "ISO 8601" for Duration.toString (both PT1.5S and
        // PT1.500S are legal spellings and neither doc pins the digit count); the engine's
        // established contract (tests/test_time.js:506) renders sub-second values with a
        // 3-digit zero-padded fraction.
        ["durationMs(1500) renders PT1.500S", "PT1.500S", () => String(durationMs(1500))],
        ["negative sign", -1, () => durationMs(-1500).sign],
        ["NaN coerces to 0 (documented)", true, () => durationMs(NaN).blank],
        ["Infinity coerces to 0 (documented)", true, () => durationMs(Infinity).blank],
        ["zero is blank", true, () => durationMs(0).blank],
        ["zero sign", 0, () => durationMs(0).sign],
        ["durationSecs(90) renders PT1M30S", "PT1M30S", () => String(durationSecs(90))],
        ["durationSecs(1.9) truncates to 1s FIRST (doc-pinned)", "PT1S", () => String(durationSecs(1.9))],
        ["durationSecs(-1.9) truncates toward zero", -1, () => durationSecs(-1.9).sign],
        ["durationSecs(9223372036854) fits (fold < int64 max, ms field exact)", "PT2562047788H54S", () => String(durationSecs(9223372036854))],
        ["durationMs(1500.9) truncates toward zero", "PT1.500S", () => String(durationMs(1500.9))],
    ], "time.Duration");
    // "a mixed-sign value throws" — ISO 8601 has no mixed-sign representation
    throwRows([
        ["mixed-sign toString", () => String(D({ months: 1, milliseconds: -1 })), Error, null],
    ], "time.Duration.refusals");
    // "An s whose millisecond fold would leave int64 throws RangeError" (dynajs.d.ts durationSecs)
    throwRows([
        ["durationSecs(1e300) throws RangeError (was: silent P0D)", () => durationSecs(1e300), Error, "overflows"],
        ["durationSecs(-1e300) throws RangeError", () => durationSecs(-1e300), Error, "overflows"],
        ["durationSecs(9.3e18) throws RangeError (past int64/1000)", () => durationSecs(9.3e18), Error, "overflows"],
        ["durationSecs(10n**30n) throws RangeError", () => durationSecs(10n ** 30n), Error, "overflows"],
    ], "time.durationSecs.overflow");
}

/* ==================== clock tables ==================== */

{
    const t0 = monotonicNano();
    const n0 = now();
    const s0 = nowSec();
    const u0 = nowUnixNano();
    const ms0 = nowMillis();
    // burn ~50ms so the monotonic-delta row has a floor (TEST-FIX: the previous version spun a
    // 25ms Date.now() budget and required >= 25ms on monotonicNano — an exact cross-clock
    // boundary; wall and CLOCK_MONOTONIC disagree slightly, so the row flaked at 24.2ms).
    const b0 = Date.now();
    while (Date.now() - b0 < 50) { /* spin */ }
    const t1 = monotonicNano();
    thunkRows([
        ["now().sec is a plausible 2023+ unix second", true, () => n0.sec > 1700000000],
        ["now().nsec in [0, 1e9)", true, () => n0.nsec >= 0 && n0.nsec < 1e9],
        ["nowSec equals now().sec within a tick", true, () => Math.abs(s0 - n0.sec) <= 1],
        ["nowUnixNano is a bigint", "bigint", () => typeof u0],
        ["nowUnixNano positive", true, () => u0 > 0n],
        ["nowNanos is the same wall clock (alias)", true, () => {
            const nn = nowNanos();
            return typeof nn === "bigint" && (nn > u0 ? nn - u0 : u0 - nn) < 1000000000n;
        }],
        ["nowMillis tracks Date.now() (same clock, doc-pinned agreement)", true, () => Math.abs(ms0 - Date.now()) < 1000],
        ["nowUnixNano/1e6 tracks nowMillis", true, () => Math.abs(Number(u0 / 1000000n) - ms0) < 1000],
        ["monotonicNano is a bigint", "bigint", () => typeof t1],
        ["monotonic never goes backwards", true, () => t1 >= t0],
        ["monotonic covers a 25ms busy loop", true, () => t1 - t0 >= 25000000n && t1 - t0 < 5000000000n],
        ["formatRFC3339(nowSec) parses back to the same second", true, () => parseRFC3339(formatRFC3339(s0)).sec === s0],
    ], "time.clocks");
    // sleep behavior via the event loop, with slack (never exact)
    const before = monotonicNano();
    await new Promise((res) => setTimeout(res, 80));
    const after = monotonicNano();
    thunkRows([
        ["80ms doze advanced monotonic >= 80ms", true, () => after - before >= 80000000n],
        ["80ms doze stayed within generous slack", true, () => after - before < 2000000000n],
    ], "time.clocks.sleep");
}

/* ==================== formatRFC3339 value table ==================== */

// d.ts/API.md pins: 0 -> "1970-01-01T00:00:00Z"; nsec emitted only when non-zero with trailing
// zeros trimmed; offsetMinutes in -1439..1439 rendered "Z" at 0 else "+/-HH:MM"; and the doc-pinned
// formatRFC3339(0, {offsetMinutes: 330}) === "1970-01-01T05:30:00+05:30".
thunkRows([
    ["epoch", "1970-01-01T00:00:00Z", () => formatRFC3339(0)],
    ["2023 timestamp", "2023-11-14T22:13:20Z", () => formatRFC3339(1700000000)],
    ["2026 timestamp (hand-derived)", "2026-08-17T10:30:00Z", () => formatRFC3339(1786962600)],
    ["0.5s fraction trimmed", "1970-01-01T00:00:00.5Z", () => formatRFC3339(0, 500000000)],
    ["1ns fraction keeps leading zeros", "1970-01-01T00:00:00.000000001Z", () => formatRFC3339(0, 1)],
    ["empty bag is the empty bag (documented)", "1970-01-01T00:00:00Z", () => formatRFC3339(0, {})],
    ["offsetMinutes 330 (doc-pinned)", "1970-01-01T05:30:00+05:30", () => formatRFC3339(0, { offsetMinutes: 330 })],
    ["negative offset crosses the date", "1969-12-31T18:30:00-05:30", () => formatRFC3339(0, { offsetMinutes: -330 })],
    ["+1439 minutes is RFC 3339's bound", "1970-01-01T23:59:00+23:59", () => formatRFC3339(0, { offsetMinutes: 1439 })],
], "time.formatRFC3339");

// Typed-slot refusals: non-number -> TypeError; non-integer/NaN/Infinity -> RangeError;
// whole number with no int64 -> RangeError "out of range"; the bag is STRICT; a third
// positional with the bag throws TypeError.
throwRows([
    ["sec as string", () => formatRFC3339("5"), TypeError, null],
    ["sec as bool", () => formatRFC3339(true), TypeError, null],
    ["sec as null", () => formatRFC3339(null), TypeError, null],
    ["sec as object", () => formatRFC3339({}), TypeError, null],
    ["sec as BigInt", () => formatRFC3339(1n), TypeError, null],
    ["sec fraction", () => formatRFC3339(2.5), RangeError, null],
    ["sec NaN", () => formatRFC3339(NaN), RangeError, null],
    ["sec Infinity", () => formatRFC3339(Infinity), RangeError, null],
    ["sec 1e300 out of range", () => formatRFC3339(1e300), RangeError, /out of range/],
    ["nsec negative", () => formatRFC3339(0, -1), RangeError, null],
    ["nsec 1e9 out of range", () => formatRFC3339(0, 1e9), RangeError, null],
    ["utc not a boolean", () => formatRFC3339(0, 0, "x"), TypeError, null],
    ["unknown bag key", () => formatRFC3339(0, { bogus: 1 }), TypeError, /bogus/],
    ["bag offsetMinutes out of range (+)", () => formatRFC3339(0, { offsetMinutes: 1440 }), RangeError, null],
    ["bag offsetMinutes out of range (-)", () => formatRFC3339(0, { offsetMinutes: -1440 }), RangeError, null],
    ["bag offsetMinutes fraction", () => formatRFC3339(0, { offsetMinutes: 330.5 }), RangeError, null],
    ["bag offsetMinutes string", () => formatRFC3339(0, { offsetMinutes: "330" }), TypeError, null],
    ["third positional with the bag", () => formatRFC3339(0, {}, 5), TypeError, null],
    ["formatUnix string in options position", () => formatUnix(0, "2006", "x"), TypeError, null],
], "time.formatRFC3339.refusals");

/* ==================== formatUnix table ==================== */

// Go-style tokens 2006 Jan Mon 01 02 15 04 05 plus %z; anything else is literal.
thunkRows([
    ["full token layout at epoch", "1970-01-01 00:00:00 Thu Jan", () => formatUnix(0, "2006-01-02 15:04:05 Mon Jan")],
    ["2023 timestamp with %z", "2023-11-14T22:13:20Z", () => formatUnix(1700000000, "2006-01-02T15:04:05%z")],
    ["fixed offset renders fields and %z", "05:30 +05:30", () => formatUnix(0, "15:04 %z", { offsetMinutes: 330 })],
    ["hand-derived 2026 date", "2026-08-17", () => formatUnix(1786962600, "2006-01-02")],
    ["weekday name token", "Thu", () => formatUnix(0, "Mon")],
    ["month name token", "Jan", () => formatUnix(0, "Jan")],
    // Regression: seconds near the int64 bounds give 12-digit years; the year token buffer
    // must hold every digit instead of truncating/smashing the stack (d.ts: formatUnix formats
    // any int64 second count via the Go-style layout). A JS number of magnitude 2^63 wraps to
    // INT64_MIN under ToInt64; the BigInt form pins the exact INT64_MAX year.
    ["INT64_MAX number seconds (ToInt64-wrapped) renders the 12-digit year", "-292277022657", () => formatUnix(9223372036854775807, "2006")],
    ["INT64_MAX BigInt seconds renders the full 12-digit year", "292277026596", () => formatUnix(9223372036854775807n, "2006")],
    ["INT64_MIN BigInt seconds renders the negative 12-digit year", "-292277022657", () => formatUnix(-9223372036854775808n, "2006")],
    ["3.2e15 seconds renders the 9-digit year", "101405933", () => formatUnix(3200000000000000, "2006")],
], "time.formatUnix");

/* ==================== parseRFC3339 table ==================== */

thunkRows([
    ["epoch", [0, 0], () => { const { sec, nsec } = parseRFC3339("1970-01-01T00:00:00Z"); return [sec, nsec]; }],
    ["2026 timestamp (hand-derived)", [1786962600, 0], () => { const { sec, nsec } = parseRFC3339("2026-08-17T10:30:00Z"); return [sec, nsec]; }],
    ["fraction to nsec", [1786962600, 500000000], () => { const r = parseRFC3339("2026-08-17T10:30:00.5Z"); return [r.sec, r.nsec]; }],
    ["offset applied (+05:30 shape from the pinned output)", [0, 0], () => { const r = parseRFC3339("1970-01-01T05:30:00+05:30"); return [r.sec, r.nsec]; }],
    ["negative offset applied", [1786982400, 0], () => { const r = parseRFC3339("2026-08-17T10:30:00-05:30"); return [r.sec, r.nsec]; }],
], "time.parseRFC3339");
throwRows([
    ["garbage", () => parseRFC3339("not a date"), SyntaxError, null],
    ["month 13 is not a valid calendar field", () => parseRFC3339("2026-13-01T00:00:00Z"), SyntaxError, null],
], "time.parseRFC3339.refusals");

/* ==================== date() table ==================== */

// "Unix seconds (UTC); an out-of-range month carries into the year (month 13 is next January)"
thunkRows([
    ["epoch", 0, () => date(1970, 1, 1)],
    ["2026-08-17 (hand-derived)", 1786924800, () => date(2026, 8, 17)],
    ["with h/mi/s", 1786962600, () => date(2026, 8, 17, 10, 30, 0)],
    ["month 13 carries into the year (documented)", date(2021, 1, 1), () => date(2020, 13, 1)],
    ["month 0 carries back", date(2019, 12, 1), () => date(2020, 0, 1)],
    ["2020-02-29 exists (leap)", date(2020, 2, 28) + 86400, () => date(2020, 2, 29)],
    ["agrees with parseRFC3339 (documented example)", parseRFC3339("2026-08-17T10:30:00Z").sec, () => date(2026, 8, 17, 10, 30, 0)],
], "time.date");

/* ==================== fromUnix table ==================== */

// "{year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday"
const FU_ROWS = [];
for (const [label, sec, fields] of [
    ["epoch", 0, { year: 1970, month: 1, day: 1, hour: 0, min: 0, sec: 0, weekday: 4, yday: 1 }],
    ["2023-11-14T22:13:20 (hand-derived)", 1700000000, { year: 2023, month: 11, day: 14, hour: 22, min: 13, sec: 20, weekday: 2, yday: 318 }],
    ["pre-epoch -1", -1, { year: 1969, month: 12, day: 31, hour: 23, min: 59, sec: 59, weekday: 3, yday: 365 }],
]) for (const f of Object.keys(fields)) FU_ROWS.push([label + " ." + f, fields[f], () => fromUnix(sec)[f]]);
thunkRows(FU_ROWS, "time.fromUnix");

/* ==================== Format class table ==================== */

{
    const f = new Format("2006-01-02");
    const fz = new Format("2006-01-02T15:04:05%z", { offsetMinutes: 330 });
    const fneg = new Format("2006-01-02", { offsetMinutes: -330 });
    const fnegz = new Format("2006-01-02T15:04:05%z", { offsetMinutes: -330 });
    thunkRows([
        ["layout readonly", "2006-01-02", () => f.layout],
        ["format 2026-08-17", "2026-08-17", () => f.format(1786924800)],
        ["parse pinned example (API.md)", 1786924800, () => f.parse("2026-08-17")],
        ["omitted fields default to the epoch (documented)", 0, () => f.parse("1970-01-01")],
        ["%z emits the instance offset", "1970-01-01T05:30:00+05:30", () => fz.format(0)],
        ["%z layout round-trips at its offset (documented)", 0, () => fz.parse(fz.format(0))],
        ["input %z wins over the instance offset (documented)", 0, () => fz.parse("1970-01-01T05:30:00+05:30")],
        // TEST-FIX: the doc pins round-tripping ONLY for %z layouts ("%z at format time emits
        // the instance's offset, so a %z layout round-trips at every offset"). fneg's layout is
        // date-only, so format(0) renders "1969-12-31" and the 18:30 time-of-day is dropped;
        // the documented rules (fields read AT the instance offset, omitted fields default to
        // the epoch midnight) give 1969-12-31T00:00:00-05:30 = -66600, not 0.
        ["negative-offset %z layout round-trips", 0, () => fnegz.parse(fnegz.format(0))],
        ["date-only layout at -330 parses midnight at that offset", -66600, () => fneg.parse(fneg.format(0))],
        ["time-only layout reads at the epoch date", 1800, () => new Format("15:04:05").parse("00:30:00")],
        // Regression: the compiled-layout formatter must render huge years in full too
        // (same int64 second domain as formatUnix per d.ts).
        ["Format formats the full 12-digit year at INT64_MAX", "292277026596", () => new Format("2006").format(9223372036854775807n)],
    ], "time.Format");
    throwRows([
        ["string in the options position", () => new Format("2006", "x"), TypeError, null],
        ["offsetMinutes out of range", () => new Format("2006", { offsetMinutes: 1500 }), RangeError, null],
        ["parse field narrower than emitted width", () => f.parse("2026-8-17"), SyntaxError, null],
        ["parse literal mismatch", () => f.parse("2026/08/17"), SyntaxError, null],
    ], "time.Format.refusals");
}

/* ==================== PlainDate table ==================== */

{
    const pd = new PlainDate(2026, 8, 17);
    thunkRows([
        ["year", 2026, () => pd.year],
        ["month", 8, () => pd.month],
        ["day", 17, () => pd.day],
        ["dayOfWeek ISO Monday=1 (hand-derived)", 1, () => pd.dayOfWeek],
        ["dayOfYear (hand-derived)", 229, () => pd.dayOfYear],
        ["daysInMonth", 31, () => pd.daysInMonth],
        ["epochDay (hand-derived)", 20682, () => pd.epochDay],
        ["1970-01-01 is a Thursday (ISO 4)", 4, () => new PlainDate(1970, 1, 1).dayOfWeek],
        ["leap Feb daysInMonth", 29, () => new PlainDate(2024, 2, 1).daysInMonth],
        ["leap inLeapYear", true, () => new PlainDate(2024, 2, 1).inLeapYear],
        ["leap daysInYear", 366, () => new PlainDate(2024, 1, 1).daysInYear],
        ["non-leap daysInMonth", 28, () => new PlainDate(2023, 2, 1).daysInMonth],
        ["non-leap inLeapYear", false, () => new PlainDate(2023, 1, 1).inLeapYear],
        ["non-leap daysInYear", 365, () => new PlainDate(2023, 1, 1).daysInYear],
        ["31 Jan + 1 month clamps (doc-pinned 28 Feb)", "2023-02-28", () => String(new PlainDate(2023, 1, 31).add(D({ months: 1 })))],
        ["leap-year clamp reaches Feb 29", "2024-02-29", () => String(new PlainDate(2024, 1, 31).add(D({ months: 1 })))],
        ["months clamp FIRST, then days (documented order)", "2023-03-01", () => String(new PlainDate(2023, 1, 31).add(D({ months: 1, days: 1 })))],
        ["add days across a month boundary", "2026-09-16", () => String(pd.add(D({ days: 30 })))],
        ["subtract days", "2026-02-28", () => String(new PlainDate(2026, 3, 1).subtract(D({ days: 1 })))],
        ["subtract months", "2026-07-17", () => String(pd.subtract(D({ months: 1 })))],
        ["until: whole months + days (hand-derived 4m15d)", 4, () => pd.until(new PlainDate(2027, 1, 1)).months],
        ["until: remaining days", 15, () => pd.until(new PlainDate(2027, 1, 1)).days],
        ["add(until(b)) === b exactly (documented)", 0, () => pd.add(pd.until(new PlainDate(2027, 1, 1))).compare(new PlainDate(2027, 1, 1))],
        ["compare less", -1, () => pd.compare(new PlainDate(2026, 12, 1))],
        ["compare equal", 0, () => pd.compare(new PlainDate(2026, 8, 17))],
        ["compare greater", 1, () => pd.compare(new PlainDate(2026, 1, 1))],
        ["toString ISO", "2026-08-17", () => String(pd)],
        ["years outside 0..9999 print signed six digits (documented)", "+012345-01-02", () => String(new PlainDate(12345, 1, 2))],
        ["parseDate strict round trip", 0, () => parseDate("2026-08-17").compare(pd)],
        ["dateFromEpochDay(0)", "1970-01-01", () => String(dateFromEpochDay(0))],
        ["dateFromEpochDay(20682)", "2026-08-17", () => String(dateFromEpochDay(20682))],
        ["dateFromEpochDay(-1)", "1969-12-31", () => String(dateFromEpochDay(-1))],
        ["epochDay round trip", 0, () => dateFromEpochDay(pd.epochDay).compare(pd)],
    ], "time.PlainDate");
    throwRows([
        ["31 February refused (doc-pinned)", () => new PlainDate(2023, 2, 29), Error, null],
        ["31 April refused", () => new PlainDate(2023, 4, 31), Error, null],
        ["month 13 refused (never rolled over)", () => new PlainDate(2023, 13, 1), Error, null],
        ["parseDate narrow field (strict ISO widths)", () => parseDate("2026-8-17"), SyntaxError, null],
        // TEST-FIX: a calendar-impossible date is syntactically valid ISO, and the engine's
        // established contract (tests/test_time.js:449) refuses parseDate with RangeError.
        ["parseDate impossible date", () => parseDate("2026-02-29"), RangeError, null],
        ["parseDate garbage", () => parseDate("garbage"), SyntaxError, null],
    ], "time.PlainDate.refusals");
}

/* ==================== PlainDateTime table ==================== */

{
    const dt = new PlainDateTime(2026, 8, 17, 23, 30);
    const a = new PlainDateTime(2026, 1, 10, 15, 0);
    const b = new PlainDateTime(2026, 2, 10, 9, 0);
    const gap = a.until(b); // doc-pinned example: 10 Jan 15:00 -> 10 Feb 09:00 is "1 month, -6h"
    thunkRows([
        ["hour", 23, () => dt.hour],
        ["minute", 30, () => dt.minute],
        ["epochDay matches the date half", 20682, () => dt.epochDay],
        ["adding time carries into the date (doc-pinned example)", "2026-08-18T01:30:00", () => String(dt.add(D({ hours: 2 })))],
        ["month add clamps like PlainDate", "2026-02-28T00:00:00", () => String(new PlainDateTime(2026, 1, 31).add(D({ months: 1 })))],
        ["until months (doc-pinned 1 month)", 1, () => gap.months],
        // TEST-FIX: Duration exposes no .milliseconds member (d.ts: years/months/days/sign/
        // blank/toString only); the documented way to observe the folded -6h remainder is the
        // exact round-trip "a.add(a.until(b)) === b" -- equivalently b.subtract(gap) === a here.
        ["until remainder: b.subtract(gap) === a (the doc-pinned -6h fold)", 0, () => b.subtract(gap).compare(a)],
        ["a.add(a.until(b)) === b exactly (documented)", 0, () => a.add(gap).compare(b)],
        ["toPlainDate halves", 0, () => dt.toPlainDate().compare(new PlainDate(2026, 8, 17))],
        ["toPlainTime halves", 0, () => dt.toPlainTime().compare(new PlainTime(23, 30))],
        ["toPlainDateTime joins losslessly (documented always)", 0, () => toPlainDateTime(dt.toPlainDate(), dt.toPlainTime()).compare(dt)],
        ["toString ISO with T, ms omitted when zero", "2026-08-18T01:30:00", () => String(dt.add(D({ hours: 2 })))],
        ["compare less", -1, () => a.compare(b)],
        ["compare greater", 1, () => b.compare(a)],
    ], "time.PlainDateTime");
    throwRows([
        ["mixed-sign until() has no ISO form (documented)", () => String(gap), Error, null],
        ["hour 24 out of range", () => new PlainDateTime(2026, 8, 17, 24), Error, null],
        ["minute 60 out of range", () => new PlainDateTime(2026, 8, 17, 1, 60), Error, null],
        ["millisecond 1000 out of range", () => new PlainDateTime(2026, 8, 17, 1, 1, 1, 1000), Error, null],
        ["toPlainDateTime refuses a non-PlainDate (documented)", () => toPlainDateTime("x", new PlainTime(1)), TypeError, null],
    ], "time.PlainDateTime.refusals");
}

/* ==================== PlainTime table ==================== */

{
    const t = new PlainTime(23, 30);
    thunkRows([
        ["add wraps at midnight (documented carry example)", 1, () => t.add(D({ hours: 2 })).hour],
        ["wrapped minute", 30, () => t.add(D({ hours: 2 })).minute],
        ["msSinceMidnight 23:30", 84600000, () => t.msSinceMidnight],
        ["msSinceMidnight fraction", 250, () => new PlainTime(0, 0, 0, 250).msSinceMidnight],
        ["subtract wraps backwards past midnight", 59, () => new PlainTime(0, 0).subtract(D({ minutes: 1 })).minute],
        ["subtract wrapped hour", 23, () => new PlainTime(0, 0).subtract(D({ minutes: 1 })).hour],
        ["compare less", -1, () => new PlainTime(1).compare(new PlainTime(2))],
        ["compare equal", 0, () => new PlainTime(1, 2).compare(new PlainTime(1, 2))],
        ["parseTime HH:MM", 0, () => parseTime("23:30").compare(new PlainTime(23, 30))],
        // TEST-FIX: the d.ts grammar is "HH:MM[:SS[.mmm]]" -- exactly three fraction digits
        // (the engine's error text and PlainTime's %03d rendering agree). ".25" is outside it.
        ["parseTime HH:MM:SS.mmm sec", 5, () => parseTime("23:30:05.250").second],
        ["parseTime .250 -> 250ms", 250, () => parseTime("23:30:05.250").millisecond],
    ], "time.PlainTime");
    throwRows([
        ["a duration in months is refused (documented)", () => new PlainTime(1).add(D({ months: 1 })), Error, null],
        // TEST-FIX: the d.ts pins no error class; the engine's rule (consistent with parseDate)
        // is SyntaxError for a malformed SHAPE, RangeError for a well-formed field out of range.
        ["hour 25 out of range", () => parseTime("25:00"), RangeError, null],
        ["second 60 out of range (leap tolerance is RFC 3339's, not this)", () => parseTime("12:00:60"), RangeError, null],
        ["a two-digit fraction is outside the [.mmm] grammar", () => parseTime("23:30:05.25"), SyntaxError, null],
    ], "time.PlainTime.refusals");
}

/* ==================== RRule + DateParser tables ==================== */

{
    // "RFC 5545 recurrence rules, UTC whole-second unix time"; fromString parses
    // "RRULE:FREQ=..." plus optional DTSTART lines.
    const rule = RRule.fromString("RRULE:FREQ=DAILY;COUNT=3", { dtstart: 0 });
    const occ = rule.all();
    thunkRows([
        ["COUNT=3 yields three occurrences", 3, () => occ.length],
        ["dtstart is the first occurrence", 0, () => occ[0].getTime()],
        ["daily steps are 86400000 ms", 86400000, () => occ[1].getTime()],
        ["third occurrence", 172800000, () => occ[2].getTime()],
        ["toString is RFC 5545 text", true, () => rule.toString().includes("FREQ=DAILY")],
    ], "time.RRule");

    const dp = new DateParser("en", { now: date(2026, 8, 17) });
    thunkRows([
        ["nothing matching parses null (documented)", null, () => dp.parse("zzz gibberish qqq")],
        ["locale readonly", "en", () => dp.locale],
        ["dayFirst is a boolean", "boolean", () => typeof dp.dayFirst],
    ], "time.DateParser");
}

/* ==================== typed slots, deeper (audit sweep) ==================== */

// d.ts: "a non-integer (2.5, NaN, Infinity) throws RangeError ... `sec`, `nsec` and
// `offsetMinutes` alike"; the legacy nsec positional "refuses exactly what the bag's
// nsec refuses"; formatUnix's offsetMinutes is "number-typed and whole like
// formatRFC3339's"; durationMs "non-integers truncate toward zero"; durationSecs
// "an s whose millisecond fold would leave int64 throws RangeError"; the
// durationMs/durationSecs slots take number | bigint.
throwRows([
    ["nsec fraction (legacy positional)", () => formatRFC3339(0, 2.5), RangeError, null],
    ["legacy nsec refuses a string like the bag's nsec", () => formatRFC3339(0, "5"), TypeError, null],
    ["formatUnix bag offsetMinutes fraction", () => formatUnix(0, "2006", { offsetMinutes: 330.5 }), RangeError, null],
    ["formatUnix bag offsetMinutes string", () => formatUnix(0, "2006", { offsetMinutes: "330" }), TypeError, null],
    // 9.3e15 s folds to 9.3e18 ms, past int64's ~9.223e18 — the documented refusal.
    // (DEVIATION noted, not pinned: durationSecs(1e300) returns P0D instead of
    // RangeError, though 1e303 ms also leaves int64.)
    ["durationSecs millisecond fold leaves int64", () => durationSecs(9.3e15), RangeError, null],
], "time.typedSlots.deep");
thunkRows([
    ["durationMs fraction truncates toward zero (documented)", "PT1.500S", () => String(durationMs(1500.9))],
    ["durationSecs accepts a bigint like the typed slot declares", String(durationSecs(90)), () => String(durationSecs(90n))],
    ["durationMs accepts a bigint", String(durationMs(1500)), () => String(durationMs(1500n))],
], "time.typedSlots.deep");

print("bb_time: all tests passed (" + n + " assertions)");
