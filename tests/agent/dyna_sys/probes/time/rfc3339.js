import { formatRFC3339, formatUnix, parseRFC3339, date,
         fromUnix, Format } from "dyna:time";
import { } from "dyna:sys";
// h.js — portable assert harness for dyna_sys black-box probes (dynajs only;
// the dyna:* modules do not exist under node). Pattern follows
// tests/agent/strnum_bb/h.js: per-failure FAIL lines, SUMMARY line, then
// RESULT PASS / RESULT FAIL; any failure throws uncaught so the process exit
// code is nonzero.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;
var __asyncPending = 0;
var __asyncFailed = false;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v.length > 120 ? v.slice(0, 117) + "..." : v);
  if (t === "number") { if (v === 0 && 1 / v < 0) return "-0"; return String(v); }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  if (t === "bigint") return v + "n";
  if (v instanceof Error) return v.name + ": " + v.message;
  try { var j = JSON.stringify(v); if (j && j.length > 200) j = j.slice(0, 197) + "..."; return j; } catch (e) { return "[unprintable]"; }
}

function __failLine(kind, msg, extra) {
  __fail++;
  __asyncFailed = true;
  __out("FAIL " + kind + " " + msg + (extra !== undefined ? " " + extra : ""));
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg, "cond=" + __show(cond));
}
function assert_true(v, msg) {
  if (v === true) { __pass++; return; }
  __failLine("assert_true", msg, "got=" + __show(v));
}
function assert_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_ne(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (!(a === e && typeof actual === typeof expected)) { __pass++; return; }
  __failLine("assert_ne", msg, "both=" + a);
}
// async variant: assertion evaluated in a promise callback; counts immediately
function assert_a(cond, msg) {
  if (cond) { __pass++; return; }
  __failLine("assert", msg);
}
function assert_a_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __failLine("assert_eq", msg, "got=" + a + " want=" + e);
}
function assert_throws(fn, kind, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (kind && name !== kind) { __failLine("assert_throws", msg, "threw=" + name + " want=" + kind); return; }
  __pass++;
}
function assert_throws_msg(fn, kind, msgSubstr, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __failLine("assert_throws_msg", msg, "no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  var m = (threw && typeof threw.message === "string") ? threw.message : "";
  if (kind && name !== kind) { __failLine("assert_throws_msg", msg, "threw=" + name + " want=" + kind); return; }
  if (msgSubstr && m.indexOf(msgSubstr) < 0) { __failLine("assert_throws_msg", msg, "msg=" + __show(m) + " want~" + msgSubstr); return; }
  __pass++;
}
// promise-rejection variant: p.then(...) style; checker receives error
function assert_rejects(p, kind, msg, checkMsgSubstr) {
  __asyncPending++;
  p.then(function (v) {
      __failLine("assert_rejects", msg, "resolved with " + __show(v));
      __asyncPending--;
      __maybeDone();
    }, function (e) {
      var name = (e && typeof e.name === "string") ? e.name : "?";
      var m = (e && typeof e.message === "string") ? e.message : "";
      if (kind && name !== kind) __failLine("assert_rejects", msg, "threw=" + name + " want=" + kind);
      else if (checkMsgSubstr && m.indexOf(checkMsgSubstr) < 0) __failLine("assert_rejects", msg, "msg=" + __show(m) + " want~" + checkMsgSubstr);
      else __pass++;
      __asyncPending--;
      __maybeDone();
    });
  return p;
}

// ---- ordered event trace (for event-order assertions) ----
var __trace = [];
function trace(ev) { __trace.push(ev); }
function trace_reset() { __trace = []; }
function assert_trace(expected, msg) {
  var a = JSON.stringify(__trace), e = JSON.stringify(expected);
  if (a === e) { __pass++; return; }
  __failLine("trace", msg, "got=" + a + " want=" + e);
}
function trace_slice() { return __trace.slice(); }

// ---- counter of async completions so summary waits for the loop ----
var __done = false;
var __doneFns = [];
function onDone(fn) { __doneFns.push(fn); }
function __maybeDone() {
  if (__done && __asyncPending === 0) {
    var fns = __doneFns; __doneFns = [];
    for (var i = 0; i < fns.length; i++) { try { fns[i](); } catch (e) { __out("FAIL onDone-handler " + e); __fail++; } }
  }
}
// finish() must be called by the probe when it has scheduled all work whose
// assertions count toward this run. summary() then waits for outstanding
// async assertions (up to timeoutMs) before emitting SUMMARY/RESULT.
var __waiters = [];
var __tag = "";
function waitAsync(ms) { __waiters.push(ms || 100); }
function summary(tag) {
  __tag = tag;
  var waitMs = 0;
  for (var i = 0; i < __waiters.length; i++) waitMs += __waiters[i];
  __done = true;
  if (__asyncPending > 0) {
    // setTimeout drives the loop until pending async assertions settle
    var step = function () {
      if (__asyncPending === 0) { __maybeDone(); finish(); return; }
      if (waitMs <= 0) {
        __out("FAIL async-timeout " + __asyncPending + " async assertion(s) never settled in " + __tag);
        __fail += __asyncPending;
        __asyncPending = 0;
        finish();
        return;
      }
      var chunk = waitMs < 50 ? waitMs : 50;
      waitMs -= chunk;
      setTimeout(step, chunk);
    };
    setTimeout(step, Math.min(50, waitMs || 50));
    return;
  }
  finish();
}
function finish() {
  __out("SUMMARY " + __tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + __tag);
  }
  __out("RESULT PASS");
}

// ---- generated probe time/rfc3339 ----

// formatRFC3339 vs python datetime (UTC oracle, baked)
const FMT = [{"sec": 0, "nsec": 0, "want": "1970-01-01T00:00:00Z"}, {"sec": 1, "nsec": 500000000, "want": "1970-01-01T00:00:01.5Z"}, {"sec": 1700000000, "nsec": 0, "want": "2023-11-14T22:13:20Z"}, {"sec": 1700000000, "nsec": 123456789, "want": "2023-11-14T22:13:20.123456789Z"}, {"sec": 951782400, "nsec": 0, "want": "2000-02-29T00:00:00Z"}, {"sec": 4102444800, "nsec": 0, "want": "2100-01-01T00:00:00Z"}];
for (const c of FMT) {
  const got = formatRFC3339(c.sec, c.nsec);
  assert_eq(got, c.want, 'formatRFC3339(' + c.sec + ',' + c.nsec + ')');
}
assert_eq(formatRFC3339(0), "1970-01-01T00:00:00Z", "epoch format (nsec omitted)");

// parseRFC3339 with offsets
const P = [["1970-01-01T00:00:00Z", 0], ["2026-08-17T10:30:00Z", 1786962600], ["2026-08-17T10:30:00+02:00", 1786955400], ["2026-08-17T10:30:00-05:30", 1786982400]];
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
