#!/usr/bin/env python3
"""gen_time.py — dyna:time probes.

Oracles:
  - python datetime/calendar: RFC 3339 format/parse samples, date()/fromUnix,
    weekday/yday/epochDay, PlainDate clamp matrices
  - a Go-faithful reference for parseDuration/durationString (units, safe-int
    split, sub-second unit choice, fraction trimming)
  - python-dateutil: RRule occurrence differentials (UTC unix/ISO)
"""
import calendar
import datetime as dt
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

try:
    from dateutil.rrule import rrule, DAILY, WEEKLY, MONTHLY, YEARLY, MO, WE, SU
    HAVE_DATEUTIL = True
except ImportError:
    HAVE_DATEUTIL = False

NS = 1
US = 1000
MS = 10**6
S = 10**9
M = 60 * S
H = 3600 * S

def go_string(ns):
    """Go Duration.String() semantics (the documented durationString)."""
    if ns == 0:
        return "0s"
    neg = ns < 0
    u = -ns if neg else ns
    if u < S:
        if u < US:
            return ("-" if neg else "") + "%dns" % u
        if u < MS:
            v = ("%f" % (u / 1e3))[:10]
            frac = ("%.3f" % (u / 1e3)).rstrip("0").rstrip(".")
            return ("-" if neg else "") + frac + "µs"
        frac = ("%.6f" % (u / 1e6)).rstrip("0").rstrip(".")
        return ("-" if neg else "") + frac + "ms"
    h = u // H
    m = (u % H) // M
    sec = (u % M) / S
    frac = sec - int(sec)
    out = ""
    if h:
        out += "%dh" % h
    if h or m:
        out += "%dm" % m
    if frac:
        s9 = "%09d" % round(frac * 1e9)
        s9 = s9.rstrip("0")
        out += ("%d.%ss" % (int(sec), s9))
    else:
        out += "%ds" % int(sec)
    return ("-" if neg else "") + out

def go_parse(s):
    """Reference for the documented parseDuration grammar. Returns
    (value, is_bigint) or None if malformed."""
    import re
    if s == "0":
        return (0, False)
    m = re.fullmatch(r"[+-]?((\d+(\.\d*)?|\.\d+)(ns|us|µs|μs|ms|s|m|h))+", s)
    if not m:
        return None
    total = 0.0
    unit_re = re.compile(r"([+-]?)(\d+(\.\d*)?|\.\d+)(ns|us|µs|μs|ms|s|m|h)")
    pos = 0
    scale = {"ns": NS, "us": US, "µs": US, "μs": US, "ms": MS, "s": S, "m": M, "h": H}
    sign = 1.0
    if s and s[0] in "+-":
        sign = -1.0 if s[0] == "-" else 1.0
    for um in unit_re.finditer(s):
        vs = um.group(2)
        u = um.group(4)
        total += float(vs) * scale[u]
    total *= sign
    if total != int(total):
        # sub-ns fractions: Go rounds; the engine matrix avoids them
        return None
    v = int(total)
    return (v, not (-(2**53) < v < 2**53))

def probe_duration():
    pairs = ["0", "1ns", "999ns", "1000ns", "1001ns", "1499ns", "1999ns",
             "1µs", "1500ns", "1ms", "1.5ms", "1500000ns", "999999ns",
             "1000000ns", "1s", "1.5s", "0.5s", "-1.5s", "1m", "90s", "1.5m",
             "1h", "1.5h", "2h45m", "300ms", "-300ms", "1h30m", "24h",
             "1h1m1s", "100000h", "1µs1ns", "2h45m30s", "-2h45m", "0.25h",
             "86400000000000ns"]
    rows = []
    for p in pairs:
        r = go_parse(p)
        assert r is not None, "matrix entry %r unparseable by reference" % p
        v, big = r
        typ = "bigint" if big else "number"
        rows.append('  ["%s", "%s", "%s"],' % (p, str(v), typ))
    emit = ['import { parseDuration, durationString, Nanosecond, Microsecond, Millisecond, Second, Minute, Hour } from "dyna:time";',
            "var PAIRS = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < PAIRS.length; i++) {
  var p = PAIRS[i];
  var got = parseDuration(p[0]);
  assert_eq(typeof got, p[2], "type " + p[0] + " -> " + typeof got);
  assert_eq(got.toString(), p[1], "value " + p[0]);
}
// durationString of every baked value must round-trip to the same ns
for (var i = 0; i < PAIRS.length; i++) {
  var v = PAIRS[i][2] === "bigint" ? BigInt(PAIRS[i][1]) : Number(PAIRS[i][1]);
  var s = durationString(v);
  var back = parseDuration(s);
  assert_eq(back.toString(), PAIRS[i][1], "roundtrip " + PAIRS[i][0] + " via " + s);
}
// exact Go strings for the sub-second ladder
var LADDER = [[0, "0s"], [999, "999ns"], [1000, "1µs"], [1499, "1.499µs"],
  [1500000, "1.5ms"], [1500000000, "1.5s"], [5400000000000, "1h30m0s"],
  [3661000000000, "1h1m1s"], [86400000000000, "24h0m0s"], [-1500000000, "-1.5s"],
  [1999, "1.999µs"], [999999, "999.999µs"], [1000000, "1ms"], [59999999999, "59.999999999s"]];
for (var i = 0; i < LADDER.length; i++)
  assert_eq(durationString(LADDER[i][0]), LADDER[i][1], "ladder " + LADDER[i][0]);
// refusals
assert_throws(function () { parseDuration("1d"); }, "SyntaxError", "day unit");
assert_throws(function () { parseDuration("1s1"); }, "SyntaxError", "trailing junk");
assert_throws(function () { parseDuration(" 1s"); }, "SyntaxError", "leading space");
assert_throws(function () { parseDuration("1s "); }, "SyntaxError", "trailing space");
assert_throws(function () { parseDuration(""); }, "SyntaxError", "empty");
assert_throws(function () { parseDuration("s"); }, "SyntaxError", "unit only");
// constants
assert_eq(Nanosecond, 1, "Nanosecond");
assert_eq(Microsecond, 1000, "Microsecond");
assert_eq(Millisecond, 1000000, "Millisecond");
assert_eq(Second, 1e9, "Second");
assert_eq(Minute, 6e10, "Minute");
assert_eq(Hour, 3.6e12, "Hour");
summary("time_duration");
""")
    emit[-1] = emit[-1]
    return write_probe("time", "duration", "\n".join(emit))


def probe_rfc3339():
    samples = [0, 1, -1, 951782400, 1700000000, 253402300799,
               946684799, 1078012800, 4107542400, 86399, 951782399]
    rows = []
    for sec in samples:
        d = dt.datetime(1970, 1, 1) + dt.timedelta(seconds=sec)
        want = "%04d-%02d-%02dT%02d:%02d:%02dZ" % (d.year, d.month, d.day,
                                                   d.hour, d.minute, d.second)
        rows.append('  [%d, "%s"],' % (sec, want))
    emit = ['import { formatRFC3339, parseRFC3339, Format, date } from "dyna:time";',
            "var SAMPLES = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < SAMPLES.length; i++) {
  assert_eq(formatRFC3339(SAMPLES[i][0]), SAMPLES[i][1], "formatRFC3339 " + SAMPLES[i][0]);
  var back = parseRFC3339(SAMPLES[i][1]);
  assert_eq(back.sec, SAMPLES[i][0], "parseRFC3339 sec " + SAMPLES[i][1]);
  assert_eq(back.nsec, 0, "parseRFC3339 nsec " + SAMPLES[i][1]);
}
// nsec rendering: trimmed trailing zeros, emitted only when non-zero
assert_eq(formatRFC3339(0, 1), "1970-01-01T00:00:00.000000001Z", "nsec 1");
assert_eq(formatRFC3339(0, 100000000), "1970-01-01T00:00:00.1Z", "nsec .1");
assert_eq(formatRFC3339(0, 123000000), "1970-01-01T00:00:00.123Z", "nsec .123");
assert_eq(formatRFC3339(0, 999999999), "1970-01-01T00:00:00.999999999Z", "nsec .999999999");
assert_eq(formatRFC3339(0).slice(-1), "Z", "no fraction when nsec omitted");
// nsec range guard
assert_throws(function () { formatRFC3339(0, 1000000000); }, "RangeError", "nsec 1e9");
assert_throws(function () { formatRFC3339(0, -1); }, "RangeError", "nsec -1");
// strictness
assert_throws(function () { parseRFC3339("2026-08-17T10:30:00"); }, "SyntaxError", "missing tz");
assert_throws(function () { parseRFC3339("2026-08-17 10:30:00Z"); }, "SyntaxError", "space separator");
assert_throws(function () { parseRFC3339("2026-02-29T00:00:00Z"); }, "SyntaxError", "non-leap Feb 29");
assert_throws(function () { parseRFC3339("2026-06-31T00:00:00Z"); }, "SyntaxError", "Jun 31");
assert_throws(function () { parseRFC3339("2026-13-01T00:00:00Z"); }, "SyntaxError", "month 13");
assert_throws(function () { parseRFC3339("2026-08-17T24:00:00Z"); }, "SyntaxError", "hour 24");
assert_throws(function () { parseRFC3339("2026-08-17T10:60:00Z"); }, "SyntaxError", "minute 60");
// tolerated: leap second literal folds into the next second
assert_eq(parseRFC3339("1999-06-30T23:59:60Z").sec, parseRFC3339("1999-07-01T00:00:00Z").sec, "leap second tolerated");
// lowercase z/t accepted (RFC 3339 grammar allows them)
assert_eq(parseRFC3339("2026-08-17t10:30:00z").sec, 1786962600, "lowercase z/t");
// offsets
assert_eq(parseRFC3339("2026-08-17T12:30:00+02:00").sec, 1786962600, "+02:00 offset");
assert_eq(parseRFC3339("2026-08-17T05:30:00-05:00").sec, 1786962600, "-05:00 offset");
// Format class: round-trip, strictness, reuse, ctor guard
var f = new Format("2006-01-02");
assert_eq(f.layout, "2006-01-02", "layout getter");
assert_eq(f.format(date(2026, 8, 17)), "2026-08-17", "Format.format");
assert_eq(f.parse("2026-08-17"), date(2026, 8, 17), "Format.parse roundtrip");
assert_throws(function () { f.parse("2026-8-17"); }, "SyntaxError", "width mismatch");
assert_throws(function () { f.parse("junk"); }, "SyntaxError", "junk");
assert_throws(function () { new Format(42); }, "TypeError", "non-string layout");
assert_throws(function () { new Format(null); }, "TypeError", "null layout");
var ft = new Format("15:04:05");
var sum = 0;
for (var k = 0; k < 100; k++) sum += ft.format(k).length;  // reuse one instance
assert_eq(sum, 800, "compiled instance reusable (100x '00:00:0X' width 8)");
// Go-layout token matrix
assert_eq(new Format("15:04:05 02/01/2006").format(1700000000), "22:13:20 14/11/2023", "token matrix");
assert_eq(new Format("Mon Jan 02 2006").format(1735689600), "Wed Jan 01 2025", "abbreviations");
summary("time_rfc3339");
""")
    return write_probe("time", "rfc3339", "\n".join(emit))


def probe_date():
    samples = [(1970, 1, 1), (1969, 12, 31), (2000, 2, 29), (2026, 8, 17),
               (1900, 3, 1), (2100, 3, 1), (2024, 2, 29), (9999, 12, 31),
               (1970, 13, 1), (1970, 0, 15), (1969, 12, 32), (2026, 2, 30)]
    rows = []
    for (y, mo, d) in samples:
        # engine date() carries out-of-range months/days like Go time.Date
        norm = dt.date(y, 1, 1) + dt.timedelta(days=(mo - 1) * 31)
        py_d = (dt.date(y, 1, 1) + dt.timedelta(days=0))
        # use calendar: month carry: month 13 -> next year Jan; day carry
        yy = y + (mo - 1) // 12
        mm = (mo - 1) % 12 + 1
        # day carry: normalize via ordinal arithmetic
        first = dt.date(yy, mm, 1)
        target = first + dt.timedelta(days=d - 1)
        epoch = dt.date(1970, 1, 1)
        esec = (target - epoch).days * 86400
        wd = (target.weekday() + 1) % 7  # engine fromUnix weekday: 0 = Sunday
        yday = target.timetuple().tm_yday
        rows.append('  [%d, %d, %d, %d, %d, %d, "%s"],'
                    % (y, mo, d, esec, wd, yday, target.isoformat()))
    emit = ['import { date, fromUnix, PlainDate, PlainDateTime, PlainTime,',
            '         parseDate, parseTime, dateFromEpochDay, Duration } from "dyna:time";',
            "var SAMPLES = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < SAMPLES.length; i++) {
  var s = SAMPLES[i];
  var esec = date(s[0], s[1], s[2]);
  assert_eq(esec, s[3], "date(" + s[0] + "," + s[1] + "," + s[2] + ")");
  var fu = fromUnix(esec);
  assert_eq(fu.year, Number(s[6].slice(0, 4)), "fromUnix year");
  assert_eq(fu.month, Number(s[6].slice(5, 7)), "fromUnix month");
  assert_eq(fu.day, Number(s[6].slice(8, 10)), "fromUnix day");
  assert_eq(fu.weekday, s[4], "fromUnix weekday (0=Sun)");
  assert_eq(fu.yday, s[5], "fromUnix yday");
}
""")
    emit.append("""
// PlainDate
var pd = new PlainDate(2026, 8, 17);
assert_eq(String(pd), "2026-08-17", "PlainDate toString");
assert_eq(pd.dayOfWeek, 1, "ISO dayOfWeek (Mon=1)");
assert_eq(pd.dayOfYear, 229, "dayOfYear");
assert_eq(pd.daysInMonth, 31, "daysInMonth");
assert_eq(pd.epochDay, 20682, "epochDay");
var leap = new PlainDate(2024, 2, 29);
assert_eq(leap.inLeapYear, true, "2024 leap");
assert_eq(leap.daysInYear, 366, "2024 daysInYear");
assert_eq(new PlainDate(2023, 2, 28).inLeapYear, false, "2023 not leap");
assert_throws(function () { new PlainDate(2023, 2, 29); }, "RangeError", "Feb 29 non-leap");
assert_throws(function () { new PlainDate(2023, 13, 1); }, "RangeError", "month 13");
// month-clamp matrix (documented: months first, clamp, then days)
var CLAMP = [
  [2023, 1, 31, 1, 0, "2023-02-28"],
  [2024, 1, 31, 1, 0, "2024-02-29"],
  [2023, 3, 31, -1, 0, "2023-02-28"],
  [2024, 3, 31, -1, 0, "2024-02-29"],
  [2023, 1, 30, 1, 0, "2023-02-28"],
  [2023, 5, 31, 1, 0, "2023-06-30"],
  [2023, 8, 31, 6, 0, "2024-02-29"],
  [2000, 1, 31, 1, 0, "2000-02-29"],
  [1900, 1, 31, 1, 0, "1900-02-28"],
];
for (var i = 0; i < CLAMP.length; i++) {
  var c = CLAMP[i];
  var moved = new PlainDate(c[0], c[1], c[2]).add(new Duration({ months: c[3], days: c[4] }));
  assert_eq(String(moved), c[5], "clamp " + c[0] + "-" + c[1] + "-" + c[2] + " " + c[3] + "mo");
}
// until: whole months + remaining days; add(until(x)) lands on x for these
var UNTIL = [
  [2023, 1, 31, 2023, 3, 30, 1, 30],
  [2024, 1, 31, 2024, 3, 30, 1, 30],
  [2023, 1, 1, 2024, 1, 1, 12, 0],
  [2023, 6, 15, 2023, 6, 20, 0, 5],
];
for (var i = 0; i < UNTIL.length; i++) {
  var u = UNTIL[i];
  var a = new PlainDate(u[0], u[1], u[2]), b = new PlainDate(u[3], u[4], u[5]);
  var d = a.until(b);
  assert_eq(d.months, u[6], "until months " + a + "->" + b);
  assert_eq(d.days, u[7], "until days " + a + "->" + b);
  assert_eq(String(a.add(d)), String(b), "until+add roundtrip " + a + "->" + b);
}
assert_eq(new PlainDate(2026, 8, 17).compare(new PlainDate(2026, 8, 17)), 0, "compare self");
assert_eq(new PlainDate(2026, 8, 17).compare(new PlainDate(2026, 1, 1)), 1, "compare gt");
assert_eq(new PlainDate(2026, 8, 17).compare(new PlainDate(2027, 1, 1)), -1, "compare lt");
assert_eq(String(parseDate("2026-08-17")), "2026-08-17", "parseDate");
assert_throws(function () { parseDate("2026-8-17"); }, "SyntaxError", "parseDate width");
assert_throws(function () { parseDate("2026-13-01"); }, "RangeError", "parseDate month 13");
assert_eq(String(dateFromEpochDay(0)), "1970-01-01", "dateFromEpochDay 0");
assert_eq(String(dateFromEpochDay(20682)), "2026-08-17", "dateFromEpochDay");
assert_eq(dateFromEpochDay(20682).epochDay, 20682, "epochDay roundtrip");
// PlainDateTime: time carries into the date
var dtv = new PlainDateTime(2026, 8, 17, 23, 30);
assert_eq(String(dtv), "2026-08-17T23:30:00", "PDT toString");
assert_eq(String(dtv.add(new Duration({ hours: 2 }))), "2026-08-18T01:30:00", "PDT carry");
assert_eq(String(new PlainDateTime(2026, 1, 2, 3, 4, 5, 250)), "2026-01-02T03:04:05.250", "PDT ms shown");
assert_eq(String(new PlainDateTime(2026, 1, 2, 3, 4, 5)), "2026-01-02T03:04:05", "PDT ms omitted");
assert_throws(function () { new PlainDateTime(2026, 1, 1, 24); }, "RangeError", "PDT hour 24");
assert_throws(function () { new PlainDateTime(2026, 1, 1, 0, 60); }, "RangeError", "PDT minute 60");
assert_eq(dtv.toPlainDate().toString(), "2026-08-17", "toPlainDate");
assert_eq(dtv.toPlainTime().toString(), "23:30:00", "toPlainTime");
assert_eq(dtv.compare(new PlainDateTime(2026, 8, 17, 0, 0)), 1, "PDT compare");
// PlainTime: wraps at midnight
assert_eq(String(new PlainTime(23, 59).add(new Duration({ minutes: 2 }))), "00:01:00", "PlainTime wrap");
assert_eq(String(new PlainTime(0, 1).subtract(new Duration({ minutes: 2 }))), "23:59:00", "PlainTime wrap back");
assert_throws(function () { new PlainTime(1, 0).add(new Duration({ months: 1 })); }, "RangeError", "PlainTime months refused");
assert_eq(new PlainTime(13, 45, 30, 250).msSinceMidnight, 49530250, "msSinceMidnight");
assert_throws(function () { new PlainTime(24); }, "RangeError", "PlainTime 24:00");
assert_eq(String(parseTime("13:45:30.250")), "13:45:30.250", "parseTime ms");
assert_eq(parseTime("13:45").msSinceMidnight, 49500000, "parseTime h:m");
assert_throws(function () { parseTime("24:00"); }, "RangeError", "parseTime 24:00");
assert_throws(function () { parseTime("aa:bb"); }, "SyntaxError", "parseTime junk");
summary("time_date");
""")
    return write_probe("time", "date", "\n".join(emit))


def probe_rrule():
    assert HAVE_DATEUTIL, "python-dateutil required for the RRule differential"
    EPOCH = dt.datetime(1970, 1, 1)
    rules = [
        ("DAILY x5", {"freq": "DAILY", "count": 5,
                      "dtstart": "2023-11-14T22:13:20Z"},
         dict(freq=DAILY, count=5, dtstart=EPOCH + dt.timedelta(seconds=1700000000))),
        ("WEEKLY MO,WE x6", {"freq": "WEEKLY", "byweekday": ["MO", "WE"], "count": 6,
                             "dtstart": "2023-11-13T00:00:00Z"},
         dict(freq=WEEKLY, byweekday=(MO, WE), count=6,
              dtstart=dt.datetime(2023, 11, 13))),
        ("MONTHLY day31 x4", {"freq": "MONTHLY", "bymonthday": 31, "count": 4,
                              "dtstart": "2023-01-01T00:00:00Z"},
         dict(freq=MONTHLY, bymonthday=31, count=4,
              dtstart=dt.datetime(2023, 1, 1))),
        ("YEARLY leap days x3", {"freq": "YEARLY", "bymonth": 2, "bymonthday": 29,
                                 "count": 3, "dtstart": "2023-01-01T00:00:00Z"},
         dict(freq=YEARLY, bymonth=2, bymonthday=29, count=3,
              dtstart=dt.datetime(2023, 1, 1))),
        ("DAILY interval 3 x4", {"freq": "DAILY", "interval": 3, "count": 4,
                                 "dtstart": 1700000000},
         dict(freq=DAILY, interval=3, count=4,
              dtstart=EPOCH + dt.timedelta(seconds=1700000000))),
        ("DAILY until", {"freq": "DAILY", "until": 1700040000,
                         "dtstart": 1700000000},
         dict(freq=DAILY, until=EPOCH + dt.timedelta(seconds=1700040000),
              dtstart=EPOCH + dt.timedelta(seconds=1700000000))),
        ("WEEKLY interval 2, SU", {"freq": "WEEKLY", "interval": 2, "byweekday": ["SU"],
                                   "count": 4, "dtstart": "2024-01-07T12:00:00Z"},
         dict(freq=WEEKLY, interval=2, byweekday=SU, count=4,
              dtstart=dt.datetime(2024, 1, 7, 12))),
    ]
    rows = []
    for name, cfg, ducfg in rules:
        rr = rrule(**ducfg)
        occs = [int((d - EPOCH).total_seconds()) for d in rr]
        # dateutil returns naive local-aware dts; dtstart given as naive UTC
        assert occs, "empty rule %s" % name
        rows.append('  ["%s", %s],' % (name, str(occs)))
    emit = ['import { RRule } from "dyna:time";',
            "var RULES = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < RULES.length; i++) {
  var name = RULES[i][0], want = RULES[i][1];
  (function (name, want) {
    var cfg;
    // configs are reconstructed by name (same matrix as the generator)
    if (name === "DAILY x5") cfg = { freq: "DAILY", count: 5, dtstart: "2023-11-14T22:13:20Z" };
    else if (name === "WEEKLY MO,WE x6") cfg = { freq: "WEEKLY", byweekday: ["MO", "WE"], count: 6, dtstart: "2023-11-13T00:00:00Z" };
    else if (name === "MONTHLY day31 x4") cfg = { freq: "MONTHLY", bymonthday: 31, count: 4, dtstart: "2023-01-01T00:00:00Z" };
    else if (name === "YEARLY leap days x3") cfg = { freq: "YEARLY", bymonth: 2, bymonthday: 29, count: 3, dtstart: "2023-01-01T00:00:00Z" };
    else if (name === "DAILY interval 3 x4") cfg = { freq: "DAILY", interval: 3, count: 4, dtstart: 1700000000 };
    else if (name === "DAILY until") cfg = { freq: "DAILY", until: 1700040000, dtstart: 1700000000 };
    else if (name === "WEEKLY interval 2, SU") cfg = { freq: "WEEKLY", interval: 2, byweekday: ["SU"], count: 4, dtstart: "2024-01-07T12:00:00Z" };
    var got = new RRule(cfg).all();
    assert_eq(got.length, want.length, "rrule " + name + " count");
    for (var k = 0; k < got.length && k < want.length; k++) {
      // all() yields ISO strings; compare against the epoch seconds
      var t = parseISO(got[k]);
      assert_eq(t, want[k], "rrule " + name + " occ[" + k + "]");
    }
  })(name, want);
}
function parseISO(s) {
  if (typeof s === "object" && s !== null && s.getTime) return s.getTime() / 1000; // Date objects
  s = String(s);
  var m = s.match(/^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})/);
  return Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) / 1000;
}
// between() is exclusive on both ends (dateutil semantics)
var r = new RRule({ freq: "DAILY", count: 5, dtstart: 1700000000 });
assert_eq(r.between(1700000000, 1700060000).length, 0, "between exclusive ends");
assert_eq(r.between(1699999999, 1700050001).length, 1, "between one hit");
// BYHOUR etc. refused rather than ignored
assert_throws(function () { new RRule({ freq: "DAILY", byhour: [9] }); }, "SyntaxError", "byhour refused");
summary("time_rrule");
""")
    return write_probe("time", "rrule", "\n".join(emit))


def probe_clocks():
    emit = ['import { now, nowMillis, nowUnixNano, monotonicNano } from "dyna:time";']
    emit.append("""
// wall clocks agree with the JS Date clock within 5s
assert_eq(now().sec > 1700000000, true, "now().sec plausible");
assert_eq(now().nsec >= 0 && now().nsec < 1e9, true, "now().nsec range");
var dnow = Date.now();
assert_true(Math.abs(nowMillis() - dnow) < 5000, "nowMillis near Date.now");
var nu = nowUnixNano();
assert_true(nu > 1700000000000000000n && nu < 3000000000000000000n, "nowUnixNano plausible");
// monotonic non-decreasing under load
var prev = monotonicNano(), decrease = 0;
for (var i = 0; i < 200000; i++) {
  var t = monotonicNano();
  if (t < prev) decrease++;
  prev = t;
}
assert_eq(decrease, 0, "monotonic never decreases");
// two wall reads are within a sane band of each other
var w1 = nowMillis(), s = 0;
for (var i = 0; i < 100000; i++) s += i % 7;
var w2 = nowMillis();
assert_true(w2 - w1 < 60000, "wall clock advanced sanely over busy loop");
// difference semantics
var a = monotonicNano(), b = monotonicNano();
assert_true(b - a >= 0n, "monotonic delta non-negative");
summary("time_clocks");
""")
    return write_probe("time", "clocks", "\n".join(emit))


if __name__ == "__main__":
    print(probe_duration())
    print(probe_rfc3339())
    print(probe_date())
    print(probe_rrule())
    print(probe_clocks())
