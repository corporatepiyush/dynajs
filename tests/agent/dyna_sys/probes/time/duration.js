import { Duration, parseDuration, durationString, Nanosecond, Microsecond,
         Millisecond, Second, Minute, Hour} from "dyna:time";
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

// ---- generated probe time/duration ----

const CASES = [["300ms", 300000000], ["1.5h", 5400000000000], ["2h45m", 9900000000000], ["0", 0], ["-1.5h", -5400000000000], ["10us", 10000], ["10\u00b5s", 10000], ["7ns", 7], ["3m20s", 200000000000], ["1h30m", 5400000000000], ["1m", 60000000000], ["2s", 2000000000]];
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
for (const bad of ["", "abc", "12", "h", "1.5.5s", "1x", "--5s", "5 s", "s5"]) {
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
