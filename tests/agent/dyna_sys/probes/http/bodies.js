import { HTTPClient} from "dyna:http";
import { getEnv } from "dyna:sys";
import { SHA256Hex } from "dyna:hash";
const PORT = getEnv("DYN_CTL_PORT");
const BASE = "http://127.0.0.1:" + PORT;
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

var __trace = [];
function trace(ev) { __trace.push(ev); }
function trace_reset() { __trace = []; }
function assert_trace(expected, msg) {
  var a = JSON.stringify(__trace), e = JSON.stringify(expected);
  if (a === e) { __pass++; return; }
  __failLine("trace", msg, "got=" + a + " want=" + e);
}
function trace_slice() { return __trace.slice(); }

var __done = false;
var __doneFns = [];
function onDone(fn) { __doneFns.push(fn); }
function __maybeDone() {
  if (__done && __asyncPending === 0) {
    var fns = __doneFns; __doneFns = [];
    for (var i = 0; i < fns.length; i++) { try { fns[i](); } catch (e) { __out("FAIL onDone-handler " + e); __fail++; } }
  }
}
var __waiters = [];
var __tag = "";
function waitAsync(ms) { __waiters.push(ms || 100); }
function summary(tag) {
  __tag = tag;
  var waitMs = 0;
  for (var i = 0; i < __waiters.length; i++) waitMs += __waiters[i];
  __done = true;
  if (__asyncPending > 0) {
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

(async () => {

const c = new HTTPClient();
c.setTimeout(8000);

{
  const r = c.get(BASE + "/body/1mb");
  assert_eq(r.status, 200, "1mb status");
  assert_eq(r.body.length, 1024 * 1024, "1mb body length, got " + r.body.length);
  const wantSha = r.headers["X-Body-Sha256"];
  assert_eq(SHA256Hex(r.body), wantSha, "sha256 of body matches server's over-the-wire bytes");
  assert_true(r.body.startsWith("000000:abcdefghijklmnopqrstuvwxyz\n"), "1mb head pattern");
}

{
  const r = c.get(BASE + "/body/empty");
  assert_eq(r.status, 200, "empty status");
  assert_eq(r.body, "", "empty body");
}

{
  const r = c.get(BASE + "/chunked");
  assert_eq(r.status, 200, "chunked status");
  assert_eq(r.body, "hello chunked world", "chunked body reassembled");
  assert_eq(r.headers["X-Chunked"], "yes", "chunked headers seen");
}
{
  const r = c.get(BASE + "/chunked-empty");
  assert_eq(r.body, "", "empty chunked body");
}

{
  const r = c.get(BASE + "/http10");
  assert_eq(r.status, 200, "http10 status");
  assert_eq(r.body, "old school", "http1.0 close-delimited body");
}

{
  const r = c.get(BASE + "/gz");
  assert_eq(r.status, 200, "gz status");
  const ce = r.headers["Content-Encoding"];
  assert_eq(ce, "gzip", "content-encoding visible");
  const rb = await c.getAsync(BASE + "/gz");
  assert_eq(SHA256Hex(rb.bodyBytes), r.headers["X-Gz-Sha"], "bodyBytes sha matches the raw gz bytes the server sent");
}

{
  const r = c.get(BASE + "/slow");
  assert_eq(r.body, "chunk0;...chunk1;...chunk2;...chunk3;...", "slow chunked body complete");
}

{
  const r = c.get(BASE + "/slow");
  assert_eq(r.body, "chunk0;...chunk1;...chunk2;...chunk3;...", "slow chunked body complete");
}

{
  c.setTimeout(400);
  const t0 = Date.now();
  let err = null;
  try { c.get(BASE + "/slowhead"); } catch (e) { err = e; }
  const dt = Date.now() - t0;
  assert_true(err !== null, "slowhead timed out");
  assert_true(dt < 3000, "timeout fired near 400ms, took " + dt + "ms");
}

summary("http.bodies");
  summary("http.bodies");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
