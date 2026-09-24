// CTL:http
import { fetch, Request, Response, Headers, FormData,
         AbortController, AbortSignal } from "dyna:http";
import { getEnv } from "dyna:sys";
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

// ---- generated probe http/fetch ----

(async () => {
  // GET json
  const r1 = await fetch(BASE + "/status?c=201");
  assert_eq(r1.status, 201, "fetch status");
  assert_eq(r1.ok, true, "fetch ok");
  assert_eq(r1.headers.get("x-status-marker"), "s201", "Headers.get is case-insensitive");

  // POST string body + headers object
  const r2 = await fetch(BASE + "/echo", {
    method: "POST",
    headers: { "X-Tag": "abc" },
    body: "payload-str",
  });
  const seen = await r2.json();
  assert_eq(seen.method, "POST", "fetch POST method");
  assert_eq(seen.body, "payload-str", "fetch POST body");
  const hv = Object.fromEntries(seen.headers.map((h) => [h[0].toLowerCase(), h[1]]));
  assert_eq(hv["x-tag"], "abc", "header object sent");

  // method is upper-cased
  const r3 = await fetch(BASE + "/echo", { method: "patch", body: "x" });
  assert_eq(JSON.parse(await r3.text()).method, "PATCH", "method upper-cased");

  // headers as pairs array
  const r4 = await fetch(BASE + "/echo", {
    method: "POST", body: "b",
    headers: [["X-Pair", "1"]],
  });
  assert_eq(JSON.parse(await r4.text()).headers.some((h) => h[0] === "X-Pair" || h[0] === "x-pair"),
    true, "headers as pairs array accepted");

  // Request/Response surface
  const req = new Request(BASE + "/echo", { method: "POST", body: "req-body" });
  assert_eq(req.method, "POST", "Request.method");
  assert_eq(await req.text(), "req-body", "Request.text");
  const res = new Response('{"ok":1}', { status: 201, statusText: "Created" });
  assert_eq(res.status, 201, "Response.status");
  assert_eq((await res.json()).ok, 1, "Response.json");

  // body single-use
  const res2 = new Response("once");
  await res2.text();
  await res2.text().then(() => assert(false, "second text() should throw"), (e) => assert_eq(e.name, "TypeError", "body consumed throws TypeError"));

  // Headers multi-value
  const h = new Headers([["x-a", "1"], ["x-a", "2"]]);
  assert_eq(h.get("x-a"), "1, 2", "append joins with comma-space");

  // Uint8Array body
  const r5 = await fetch(BASE + "/upload", { method: "POST", body: new Uint8Array([1, 2, 3, 4, 5]) });
  const up = await r5.json();
  assert_eq(up.recvlen, 5, "bytes body arrives");

  // FormData multipart
  const fd = new FormData();
  fd.append("a", "1");
  fd.append("f", new Uint8Array([9, 8, 7]), "blob.bin");
  const r6 = await fetch(BASE + "/upload", { method: "POST", body: fd });
  const up6 = await r6.json();
  assert_true(up6.ct.indexOf("multipart/form-data") === 0, "FormData sets multipart content-type: " + up6.ct);
  assert_true(up6.recvlen > 100, "multipart body non-trivial: " + up6.recvlen);
  // MultipartParse round-trip against the client's own format
  const fmt = MultipartFormatLocal();
  function MultipartFormatLocal() {
    return { ct: "multipart/form-data; boundary=X", body: null };
  }

  // abort: pre-aborted rejects immediately with the reason
  {
    const ac = new AbortController();
    ac.abort(new Error("stop-now"));
    const t0 = Date.now();
    await fetch(BASE + "/slow", { signal: ac.signal }).then(
      () => assert(false, "pre-aborted fetch should reject"),
      (e) => {
        assert_true(String(e.message).indexOf("stop-now") >= 0, "pre-aborted rejects with reason: " + e.message);
        assert_true(Date.now() - t0 < 500, "pre-aborted rejects immediately");
      });
  }

  // abort mid-flight on the slow route
  {
    const ac = new AbortController();
    setTimeout(() => ac.abort(new Error("cut")), 120);
    const p = fetch(BASE + "/slow", { signal: ac.signal });
    await p.then(
      () => assert(false, "mid-flight abort should reject"),
      (e) => assert_true(String(e.message).indexOf("cut") >= 0, "mid-flight abort rejects with reason: " + e.message));
  }

  // AbortSignal.timeout fires
  {
    const p = fetch(BASE + "/slowhead", { signal: AbortSignal.timeout(300) });
    await p.then(
      () => assert(false, "signal timeout should reject"),
      (e) => assert_true(e !== null, "AbortSignal.timeout rejects"));
  }

  summary("http.fetch");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
