#!/usr/bin/env python3
"""gen_time.py — dyna:time probes. Oracles: python datetime/strftime for
RFC3339 & layouts; Go-layout truths hand-baked; monotonic/wall invariants."""
import json
import time as pytime
from datetime import datetime, timezone, timedelta
from probe_lib import emit

IMP = '''import { Duration, parseDuration, durationString, Nanosecond, Microsecond,
         Millisecond, Second, Minute, Hour, now, nowMillis, nowUnixNano,
         monotonicNano, formatRFC3339, formatUnix, parseRFC3339, date,
         fromUnix, Format } from "dyna:time";
import { Exec } from "dyna:sys";
'''

# RFC3339 matrix: python datetime is the oracle (UTC)
RFC_CASES = [
    (0, 0), (1, 500000000), (1700000000, 0), (1700000000, 123456789),
    (951782400, 0), (4102444800, 0),
]
rfc_baked = []
for sec, nsec in RFC_CASES:
    dt = datetime.fromtimestamp(sec, tz=timezone.utc)
    base = dt.strftime("%Y-%m-%dT%H:%M:%S")
    text = base + (".%09dZ" % nsec).rstrip("0").replace(".Z", ".000000001Z") if nsec else base + "Z"
    if nsec:
        frac = ("%09d" % nsec).rstrip("0")
        text = base + "." + frac + "Z"
    rfc_baked.append({"sec": sec, "nsec": nsec, "want": text})

# parseRFC3339 with offsets: (text, expected_utc_sec)
KNOWN_DT = datetime(2026, 8, 17, 10, 30, tzinfo=timezone.utc)
KNOWN_TS = str(int(KNOWN_DT.timestamp()))
PARSE_CASES = [
    ("1970-01-01T00:00:00Z", 0),
    ("2026-08-17T10:30:00Z", int(KNOWN_DT.timestamp())),
    ("2026-08-17T10:30:00+02:00", int((KNOWN_DT - timedelta(hours=2)).timestamp())),
    ("2026-08-17T10:30:00-05:30", int((KNOWN_DT + timedelta(hours=5, minutes=30)).timestamp())),
]

DUR_CASES = [
    ("300ms", 300 * 10**6), ("1.5h", 5400 * 10**9), ("2h45m", 9900 * 10**9),
    ("0", 0), ("-1.5h", -5400 * 10**9), ("10us", 10 * 10**3),
    ("10µs", 10 * 10**3), ("7ns", 7), ("3m20s", 200 * 10**9),
    ("1h30m", 5400 * 10**9), ("1m", 60 * 10**9), ("2s", 2 * 10**9),
]
DUR_BAD = ["", "abc", "12", "h", "1.5.5s", "1x", "--5s", "5 s", "s5"]

emit("time", "duration", IMP, r'''
const CASES = __DUR_CASES__;
for (const [text, ns] of CASES) {
  const v = parseDuration(text);
  const norm = typeof v === "bigint" ? Number(v) : v;
  assert_eq(norm, ns, 'parseDuration("' + text + '")');
}
// safe-integer rule: sub-day values are Numbers, huge ones BigInt
assert_eq(typeof parseDuration("300ms"), "number", "safe magnitude -> number");
{
  const big = parseDuration("100000h");  // 3.6e17 ns > 2^53
  assert_true(typeof big === "bigint" || typeof big === "number", "100000h type: " + typeof big);
  if (typeof big === "bigint") assert_eq(big, 360000000000000000n, "100000h exact ns via BigInt");
  else assert_eq(big, 3.6e17, "100000h ns");
}
// inverse
for (const [text, ns] of CASES) {
  if (ns === 0) { assert_eq(durationString(0), "0s", "durationString(0)"); continue; }
  const s = durationString(ns);
  const back = parseDuration(s);
  const n2 = typeof back === "bigint" ? Number(back) : back;
  assert_eq(n2, ns, 'round-trip durationString(' + ns + ') = "' + s + '"');
}
assert_eq(durationString(0), "0s", "zero renders 0s");
assert_eq(durationString(1500000000), "1.5s", "fraction trimmed");
// refusals
for (const bad of __DUR_BAD__) {
  assert_throws(() => parseDuration(bad), "SyntaxError", 'parseDuration(' + JSON.stringify(bad) + ') throws SyntaxError');
}
// constants
assert_eq(Nanosecond, 1, "Nanosecond");
assert_eq(Microsecond, 1000, "Microsecond");
assert_eq(Millisecond, 10**6, "Millisecond");
assert_eq(Second, 10**9, "Second");
assert_eq(Minute, 60 * 10**9, "Minute");
assert_eq(Hour, 3600 * 10**9, "Hour");

// Duration class: folding, ISO text, sign
const d = new Duration({ hours: 1, minutes: 30 });
assert_eq(String(d), "PT1H30M", "ISO duration");
const m = new Duration({ months: 14, days: 3 });
assert_eq(m.years, 1, "months fold into years");
assert_eq(m.months, 2, "remainder months");
assert_eq(m.days, 3, "days stay days");
const z = new Duration({ seconds: 0 });
assert_eq(z.blank, true, "all-zero is blank");
assert_eq(z.sign, 0, "zero sign");
const neg = new Duration({ seconds: -5 });
assert_eq(neg.sign, -1, "negative sign");
summary("time.duration");
'''.replace("__DUR_CASES__", json.dumps(DUR_CASES)).replace("__DUR_BAD__", json.dumps(DUR_BAD)))

GEN_MS = None
emit("time", "clocks", IMP, r'''
// monotonic non-decreasing, including under load
let prev = monotonicNano();
for (let i = 0; i < 1000; i++) {
  const t = monotonicNano();
  if (t < prev) { assert(false, "monotonicNano went backwards at i=" + i); prev = t; break; }
  prev = t;
  if (i % 100 === 0) { let x = 0; for (let j = 0; j < 20000; j++) x += j * j; if (x < 0) throw new Error("no"); }
}
assert(true, "monotonic non-decreasing under load");

// monotonic measures a real sleep
const t0 = monotonicNano();
{
  // busy+timer wait: the loop must advance by >= 50ms as timers fire
}
setTimeout(() => {
  const dt = Number(monotonicNano() - t0) / 1e6;
  assert_true(dt >= 45, "50ms timer >= 45ms on the monotonic clock, got " + dt.toFixed(1) + "ms");
  assert_true(dt < 2000, "50ms timer fired promptly, got " + dt.toFixed(1) + "ms");

  // wall clock sanity vs LIVE python time.time() (differential at probe time)
  const pyMs = parseInt(Exec("python3", ["-c", "import time; print(int(time.time()*1000))"]).stdout.trim());
  const ms = nowMillis();
  assert_true(Math.abs(ms - pyMs) < 5000,
    "nowMillis within 5s of python time.time(), delta=" + (ms - pyMs));

  // consistency between the three wall clocks
  const n = now();
  const nano = nowUnixNano();
  const ms2 = nowMillis();
  assert_true(Math.abs(Number(nano) / 1e6 - ms2) < 50, "nowUnixNano and nowMillis agree");
  assert_true(Math.abs((n.sec * 1000 + Math.floor(n.nsec / 1e6)) - ms2) < 50, "now() and nowMillis() agree");
  assert_true(n.nsec >= 0 && n.nsec < 1e9, "nsec in range");

  // summary deferred to the timer: event trace of timer ordering lives in time.timers
  summary("time.clocks");
}, 50);
'''.replace("__BAKE_LO__", str(int((pytime.time() - 600) * 1000)))
  .replace("__BAKE_HI__", str(int((pytime.time() + 600) * 1000))))

emit("time", "rfc3339", IMP, r'''
// formatRFC3339 vs python datetime (UTC oracle, baked)
const FMT = __RFC_BAKED__;
for (const c of FMT) {
  const got = formatRFC3339(c.sec, c.nsec);
  assert_eq(got, c.want, 'formatRFC3339(' + c.sec + ',' + c.nsec + ')');
}
assert_eq(formatRFC3339(0), "1970-01-01T00:00:00Z", "epoch format (nsec omitted)");

// parseRFC3339 with offsets
const P = __PARSE_CASES__;
for (const [text, sec] of P) {
  const r = parseRFC3339(text);
  assert_eq(r.sec, sec, 'parseRFC3339("' + text + '").sec');
}

// round-trip property
for (const c of FMT) {
  const r = parseRFC3339(c.want);
  assert_eq(r.sec, c.sec, "round-trip sec " + c.sec);
}

// strictness
for (const bad of ["not a date", "2026-13-01T00:00:00Z", "2026-08-17 10:30:00Z", "2026-08-17T10:30:00", "20260817T103000Z"]) {
  assert_throws(() => parseRFC3339(bad), null, 'parseRFC3339 rejects ' + JSON.stringify(bad));
}

// formatUnix Go-layout tokens vs hand-baked truths (epoch and known date)
assert_eq(formatUnix(0, "2006-01-02 15:04:05 Mon Jan"), "1970-01-01 00:00:00 Thu Jan", "epoch layout");
assert_eq(formatUnix(1786962600, "2006-01-02"), "2026-08-17", "date token");
assert_eq(formatUnix(1786962600, "15:04:05"), "10:30:00", "time token");
assert_eq(formatUnix(0, "Jan 02 Mon"), "Jan 01 Thu", "month/day/weekday tokens");

// Format: compiled layout, format/parse round-trip
const f = new Format("2006-01-02");
const ts = date(2026, 8, 17);
assert_eq(f.format(ts), "2026-08-17", "Format.format");
assert_eq(f.parse("2026-08-17"), ts, "Format.parse round-trip");
assert_eq(f.layout, "2006-01-02", "layout accessor");
assert_throws(() => f.parse("2026-8-17"), null, "parse is strict about field width");

// date()/fromUnix() civil facts
assert_eq(fromUnix(0).year, 1970, "fromUnix year");
assert_eq(fromUnix(0).month, 1, "fromUnix month");
assert_eq(fromUnix(0).day, 1, "fromUnix day");
assert_eq(fromUnix(0).weekday, 4, "epoch was a Thursday (0=Sunday)");
assert_eq(date(2026, 8, 17, 10, 30, 0), 1786962600, "date() constructs UTC seconds");
// month 13 carries into next year
assert_eq(date(2026, 13, 1), date(2027, 1, 1), "month 13 carries");
assert_eq(parseRFC3339("2026-08-17T10:30:00Z").sec, date(2026, 8, 17, 10, 30, 0), "date() agrees with parse");
summary("time.rfc3339");
'''.replace("__RFC_BAKED__", json.dumps(rfc_baked)).replace("__PARSE_CASES__", json.dumps(PARSE_CASES)))

emit("time", "timers", IMP, r'''
// multiple timers: firing order is deadline order, even when registered
// in reverse; equal deadlines fire in registration order.
trace_reset();
const t3 = setTimeout(() => trace("t3"), 30);
const t1 = setTimeout(() => trace("t1"), 10);
const t2a = setTimeout(() => trace("t2a"), 20);
const t2b = setTimeout(() => trace("t2b"), 20);
setTimeout(() => {
  assert_trace(["t1", "t2a", "t2b", "t3"], "timers fire in deadline then registration order");
  // clearTimeout prevents firing
  trace_reset();
  const a = setTimeout(() => trace("a"), 10);
  clearTimeout(a);
  setTimeout(() => {
    assert_trace([], "cleared timer never fired");
    summary("time.timers");
  }, 40);
}, 80);
''')
print("gen_time: 4 probes")
