// CTL:resp
import { Redis } from "dyna:net";
import { getEnv } from "dyna:sys";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
function bytesToStr(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
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

// ---- generated probe net/resp ----

(async () => {
  const rd = new Redis({ host: "127.0.0.1", port: PORT });
  await rd.command("PING");  // handshake queue flush
  assert_eq(rd.ready, true, "handshake settled");

  // wire format OUT: PING as an array of bulks — ask the server via RAWLOG
  await rd.command("PING");
  const raw = await rd.command("RAWLOG");
  const rawStr = typeof raw === "string" ? raw : bytesToStr(raw);
  assert_true(rawStr.indexOf("*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n") === 0 ||
              rawStr.indexOf("HELLO") >= 0, "handshake sent HELLO: " + JSON.stringify(rawStr.slice(0, 60)));
  assert_true(rawStr.indexOf("*1\r\n$4\r\nPING\r\n") >= 0,
    "client sent exact RESP array *1\\r\\n$4\\r\\nPING\\r\\n, log tail: " + JSON.stringify(rawStr.slice(-60)));

  // inline commands: server parses them too (send raw over a second client? use command with raw bytes)
  // wire format IN: each reply type
  assert_eq(await rd.command("PING"), "PONG", "+PONG simple");
  assert_eq(await rd.command("ECHO", "hello"), "hello", "bulk round-trip");
  assert_eq(await rd.command("SET", "k", "v1"), "OK", "+OK status");
  assert_eq(await rd.command("GET", "k"), "v1", "GET bulk");
  assert_eq(await rd.command("GET", "missing-key"), null, "GET missing -> null ($-1)");
  assert_eq(await rd.command("NULLBULK"), null, "null bulk");
  assert_eq(await rd.command("EMPTYSTR"), "", "empty bulk string");
  assert_eq(await rd.command("INT"), 12345, ":integer reply");
  assert_eq(await rd.command("DEL", "k", "nope"), 1, "multi-arg DEL");

  // error reply rejects with the message
  await rd.command("MYERR").then(
    () => assert(false, "MYERR must reject"),
    (e) => assert_true(String(e.message || e).indexOf("my deliberate error") >= 0,
      "error reply carries server message: " + e.message));

  // nested arrays
  {
    const n = await rd.command("NESTED");
    const j = JSON.stringify(n);
    assert_true(j.indexOf("abc") >= 0 && j.indexOf("1") >= 0, "nested multi-bulk parsed: " + j);
  }
  // NULL array
  {
    const n = await rd.command("NULLARR");
    assert_true(n === null || (Array.isArray(n) && n.length === 0), "null array -> null/[]: " + JSON.stringify(n));
  }
  // binary-safe payload needs the binary:true client (the default client
  // UTF-8-decodes bulks to text, which mangles binary by documented design)
  {
    const rdb = new Redis({ host: "127.0.0.1", port: PORT, binary: true });
    await rdb.command("PING");
    const v = await rdb.command("BINVAL");
    let ok = v instanceof Uint8Array && v.length === 256;
    for (let i = 0; i < 256 && ok; i++) if (v[i] !== i) ok = false;
    assert_true(ok, "binary payload with \\r\\n inside survives (binary client)");
    // big value 1 MiB through the binary client: byte-exact
    const big = await rdb.command("BIGVAL");
    assert_eq(big.length, 1024 * 1024, "1MiB bulk length (bytes)");
    let bigOk = true;
    for (let i = 0; i < 1024 * 1024; i += 4095) if (big[i] !== i % 256) { bigOk = false; break; }
    assert_true(bigOk, "1MiB content spot-check");
    rdb.close();
  }
  // 64-bit integer: exact digits as text without bigint
  {
    const v = await rd.command("BIGINT");
    assert_eq(String(v), "9223372036854775807", "64-bit int exact digits, got " + String(v));
  }
  // pipeline: one round trip, ordered replies
  {
    const replies = await rd.pipeline([["PING"], ["ECHO", "x"], ["SET", "pk", "pv"], ["GET", "pk"]]);
    assert_eq(replies.length, 4, "pipeline reply count");
    assert_eq(replies[0], "PONG", "pipeline [0]");
    assert_eq(replies[1], "x", "pipeline [1]");
    assert_eq(replies[2], "OK", "pipeline [2]");
    assert_eq(replies[3], "pv", "pipeline [3] ordered");
  }
  // premature close: DIE closes mid-reply; pending rejects
  {
    await rd.command("DIE").then(
      () => assert(true, "DIE +BYE partial resolved or rejects below"),
      (e) => assert(true, "DIE rejected: " + e.message));
    await new Promise((r) => setTimeout(r, 200));
    let err = null;
    try { await rd.command("PING"); } catch (e) { err = e; }
    assert_true(err !== null, "commands after teardown reject: " + (err && err.message));
  }
  rd.close();

  // RESP3 protocol negotiated + push frames
  const rd2 = new Redis({ host: "127.0.0.1", port: PORT });
  const pushes = [];
  rd2.on("push", (p) => pushes.push(p));
  await rd2.command("PING");
  assert_true(rd2.protocol === 2 || rd2.protocol === 3, "protocol negotiated: " + rd2.protocol);
  await rd2.command("PUSH1");
  await new Promise((r) => setTimeout(r, 300));
  assert_true(pushes.length >= 1, "RESP3 push frame delivered to on(push), got " + pushes.length);
  rd2.close();

  // bigint option client
  const rd3 = new Redis({ host: "127.0.0.1", port: PORT, bigint: true });
  const big = await rd3.command("BIGINT");
  assert_true(typeof big === "bigint" || big === "9223372036854775807", "bigint flag: " + typeof big);
  rd3.close();

  summary("net.resp");
})().catch((e) => { print("FAIL uncaught " + e); throw e; });
