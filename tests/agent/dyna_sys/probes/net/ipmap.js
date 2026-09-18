import { parseAddr, parsePrefix, canonical, isValid, compareAddr, contains,
         masked, isLoopback, isPrivate, isMulticast, isUnspecified, Prefix,
         RateLimiter, Metrics } from "dyna:net";
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

// ---- generated probe net/ipmap ----

const CASES = [{"t": "192.168.1.1", "canon": "192.168.1.1", "is4": true, "is6": false, "loopback": false, "private": true, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "10.0.0.1", "canon": "10.0.0.1", "is4": true, "is6": false, "loopback": false, "private": true, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "127.0.0.1", "canon": "127.0.0.1", "is4": true, "is6": false, "loopback": true, "private": false, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "8.8.8.8", "canon": "8.8.8.8", "is4": true, "is6": false, "loopback": false, "private": false, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "169.254.5.5", "canon": "169.254.5.5", "is4": true, "is6": false, "loopback": false, "private": false, "multicast": false, "unspecified": false, "linklocal": true}, {"t": "224.0.0.1", "canon": "224.0.0.1", "is4": true, "is6": false, "loopback": false, "private": false, "multicast": true, "unspecified": false, "linklocal": false}, {"t": "0.0.0.0", "canon": "0.0.0.0", "is4": true, "is6": false, "loopback": false, "private": false, "multicast": false, "unspecified": true, "linklocal": false}, {"t": "::1", "canon": "::1", "is4": false, "is6": true, "loopback": true, "private": false, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "::", "canon": "::", "is4": false, "is6": true, "loopback": false, "private": false, "multicast": false, "unspecified": true, "linklocal": false}, {"t": "fe80::1", "canon": "fe80::1", "is4": false, "is6": true, "loopback": false, "private": false, "multicast": false, "unspecified": false, "linklocal": true}, {"t": "ff02::1", "canon": "ff02::1", "is4": false, "is6": true, "loopback": false, "private": false, "multicast": true, "unspecified": false, "linklocal": false}, {"t": "2001:db8::ff00:42:8329", "canon": "2001:db8::ff00:42:8329", "is4": false, "is6": true, "loopback": false, "private": false, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "::ffff:10.0.0.1", "canon": "::ffff:10.0.0.1", "is4": false, "is6": true, "loopback": false, "private": true, "multicast": false, "unspecified": false, "linklocal": false}, {"t": "2001:db8::1", "canon": "2001:db8::1", "is4": false, "is6": true, "loopback": false, "private": false, "multicast": false, "unspecified": false, "linklocal": false}];
for (const c of CASES) {
  if (!isValid(c.t)) { assert(false, "isValid false for " + c.t); continue; }
  const a = parseAddr(c.t);
  assert_eq(a.string, c.canon, "canonical(" + c.t + ")");
  assert_eq(a.is4, c.is4, "is4 " + c.t);
  assert_eq(a.is6, c.is6, "is6 " + c.t);
  assert_eq(a.bytes.length, c.is4 ? 4 : 16, "bytes length " + c.t);
  assert_eq(isLoopback(c.t), c.loopback, "isLoopback " + c.t);
  assert_eq(isPrivate(c.t), c.private, "isPrivate " + c.t);
  assert_eq(isMulticast(c.t), c.multicast, "isMulticast " + c.t);
  assert_eq(isUnspecified(c.t), c.unspecified, "isUnspecified " + c.t);
}
assert_eq(isValid("999.1.1.1"), false, "isValid rejects 999");
assert_eq(isValid("nope"), false, "isValid rejects junk");
assert_eq(isValid(42), false, "isValid non-string false");
assert_throws(() => parseAddr("999.1.1.1"), "TypeError", "parseAddr malformed throws");
assert_throws(() => parseAddr("fe80::1%eth0"), "TypeError", "zone refused");

assert_eq(canonical("0:0:0:0:0:0:0:1"), "::1", "compression");
assert_eq(canonical("2001:0db8:0000:0000:0000:ff00:0042:8329"), "2001:db8::ff00:42:8329", "leftmost-longest compression");

// prefix math vs python
assert_eq(masked("192.168.1.55/24"), "192.168.1.0", "masked v4");
assert_eq(masked("2001:db8:f00::/32"), "2001:db8::", "masked v6");
assert_eq(JSON.stringify(parsePrefix("10.0.0.0/8")), '{"addr":"10.0.0.0","bits":8}', "parsePrefix");
assert_eq(contains("10.0.0.0/8", "10.1.2.3"), true, "contains true");
assert_eq(contains("10.0.0.0/8", "11.1.2.3"), false, "contains false");
assert_eq(compareAddr("10.0.0.1", "192.168.1.1"), -1, "v4 sorts before v4");
assert_eq(compareAddr("1.1.1.1", "1.1.1.1"), 0, "compare equal");

// compiled Prefix
const p = new Prefix("10.0.0.0/8");
assert_eq(p.contains("10.1.2.3"), true, "Prefix.contains in");
assert_eq(p.contains("11.0.0.1"), false, "Prefix.contains out");
assert_eq(p.contains("not-an-ip"), false, "unparseable is false");
assert_eq(p.bits, 8, "Prefix.bits");
assert_eq(p.masked, "10.0.0.0", "Prefix.masked");
assert_eq(p.isIPv4, true, "Prefix.isIPv4");
assert_eq(new Prefix("10.0.0.0/8").overlaps(new Prefix("10.5.0.0/16")), true, "overlaps true");
assert_eq(new Prefix("10.0.0.0/8").overlaps(new Prefix("192.168.0.0/16")), false, "overlaps false");
assert_throws(() => new Prefix("nonsense"), "TypeError", "bad CIDR throws");

// RateLimiter
const rl = new RateLimiter({ tokensPerSec: 5, burst: 3 });
assert_eq(rl.allow("a"), true, "allow 1");
assert_eq(rl.allow("a"), true, "allow 2");
assert_eq(rl.allow("a"), true, "allow 3");
assert_eq(rl.allow("a"), false, "deny past burst");
assert_eq(rl.tokens("a"), 0, "tokens drained");
const stats = rl.stats;
assert_eq(stats.allowed, 3, "stats.allowed");
assert_eq(stats.denied, 1, "stats.denied");
assert_eq(stats.burst, 3, "stats.burst");
rl.reset("a");
assert_eq(rl.allow("a"), true, "reset restores");
assert_throws(() => new RateLimiter({}), null, "tokensPerSec required");
assert_throws(() => rl.allow("a", 0), null, "non-positive cost refused");
assert_throws(() => rl.allow("a", -1), null, "negative cost refused");

// Metrics
Metrics.reset();
Metrics.counter("http_requests_total", 2, { method: "GET" });
Metrics.counter("http_requests_total");
Metrics.gauge("queue_depth", 3);
const lines = Metrics.scrape().split("\n")
  .filter((l) => l.startsWith("http_requests_total") || l.startsWith("queue_depth"));
assert_eq(lines.join("|"), 'http_requests_total{method="GET"} 2|http_requests_total 1|queue_depth 3',
  "prometheus exposition: " + JSON.stringify(lines));
Metrics.histogram("latency", 0.007);
assert_true(Metrics.scrape().indexOf("latency_bucket") >= 0, "histogram buckets exposed");
assert_throws(() => Metrics.counter("http_requests_total", -1), "RangeError", "negative increment refused");
summary("net.ipmap");
