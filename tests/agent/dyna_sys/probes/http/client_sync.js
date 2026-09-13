// CTL:http
import { HTTPClient} from "dyna:http";
import { getEnv } from "dyna:sys";
import { SHA256Hex } from "dyna:hash";
const PORT = getEnv("DYN_CTL_PORT");
const BASE = "http://127.0.0.1:" + PORT;
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

// ---- generated probe http/client_sync ----

const c = new HTTPClient();
c.setTimeout(5000);

// GET: server /echo reports exactly what it received
{
  const r = c.get(BASE + "/echo?probe=1");
  assert_eq(r.status, 200, "GET status");
  assert_eq(r.ok, true, "GET ok");
  const seen = JSON.parse(r.body);
  assert_eq(seen.method, "GET", "server saw GET");
  assert_eq(seen.path, "/echo?probe=1", "server saw path+query verbatim");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv.host, "127.0.0.1:" + PORT, "client sent Host header");
}

// POST body fidelity (string + bytes)
{
  const r = c.post(BASE + "/echo", "hello=world&x=1", { "Content-Type": "application/x-www-form-urlencoded" });
  const seen = JSON.parse(r.body);
  assert_eq(seen.method, "POST", "server saw POST");
  assert_eq(seen.body, "hello=world&x=1", "body bytes arrived");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv["content-type"], "application/x-www-form-urlencoded", "custom content-type sent");
  assert_eq(hv["content-length"], "15", "content-length matches body bytes");
}
{
  const bytes = new Uint8Array(256);
  for (let i = 0; i < 256; i++) bytes[i] = i;
  const r = c.post(BASE + "/upload", bytes);
  assert_eq(r.status, 201, "bytes POST status");
  const seen = JSON.parse(r.body);
  assert_eq(seen.recvlen, 256, "all 256 body bytes arrived (binary-safe)");
  assert_eq(seen.sha, SHA256Hex(bytes), "sha256 of received bytes matches");
}

// PUT / DELETE / HEAD via request()
{
  const p = c.request("PUT", BASE + "/echo", "put-body");
  assert_eq(JSON.parse(p.body).method, "PUT", "request(PUT)");
  const d = c.request("DELETE", BASE + "/echo");
  assert_eq(JSON.parse(d.body).method, "DELETE", "request(DELETE)");
  const h = c.request("HEAD", BASE + "/echo");
  assert_eq(h.status, 200, "HEAD status");
  assert_eq(h.body, "", "HEAD has empty body");
}

// status matrix: classes 2xx/3xx/4xx/5xx
for (const code of [200, 201, 204, 301, 404, 418, 500, 503]) {
  const r = c.get(BASE + "/status?c=" + code);
  assert_eq(r.status, code, "status " + code + " preserved");
  assert_eq(r.ok, code >= 200 && code < 300, "ok for " + code);
  assert_eq(r.statusText.length > 0, true, "statusText present for " + code);
  if (code !== 204) assert_eq(r.body, "status=" + code + "\n", "body for " + code);
  else assert_eq(r.body, "", "204 has no body");
}

// response headers: exact names the server sent; duplicates collapsed or joined (impl-defined, assert stable)
{
  const r = c.get(BASE + "/headers");
  assert_eq(r.headers["X-Weird-Case"], "PreserveMe-123", "header name case preserved verbatim");
  assert_eq(r.headers["Empty-Header"], "", "empty header value preserved");
  // duplicates are impl-defined for the raw client's plain-object headers;
  // the implementation resolves them LAST-WINS (curl-like), assert stability
  assert_eq(r.headers["Set-Cookie"], "b=2; Path=/", "duplicate Set-Cookie resolves last-wins (impl-defined)");
  assert_eq(r.headers["X-Multi"], "two", "duplicate X-Multi resolves last-wins (impl-defined)");
}

// URL refusals before any connection
assert_throws_msg(() => c.get("not a url"), null, "bad URL", "no scheme refused");
assert_throws_msg(() => c.get("ftp://host/"), null, "bad URL", "ftp refused");
assert_throws_msg(() => c.get(BASE + "/x\r\nInjected: y"), null, "bad URL", "CRLF injection refused");

// close/dispose lifecycle
const c2 = new HTTPClient();
assert_eq(c2.closed, false, "fresh client open");
c2.close();
assert_eq(c2.closed, true, "closed after close()");

summary("http.client_sync");
