// CTL:tcp
import { TCPServer, UDPSocket} from "dyna:net";
import { getEnv } from "dyna:sys";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
const HOST = "127.0.0.1";
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

// ---- generated probe net/tcp ----

const results = {};
function once(mode, payload, name) {
  return new Promise((resolve) => {
    const events = [];
    const cli = TCPServer.connect({ host: HOST, port: PORT },
      { connect: (c, err) => {
          if (!c) { events.push("err:" + err); resolve({ events, cli: null }); return; }
          events.push("connect");
          c.write(payload);
        },
        data: (c, b) => { events.push("data:" + bytesToStr(b)); },
        close: (c) => { events.push("close"); resolve({ events, cli }); }});
    cli._name = name;
    setTimeout(() => resolve({ events, cli }), 3000); // watchdog: never hang
  });
}
const P = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  // B: binary-safe echo, mode byte + all 256 byte values (incl \r\n and NUL)
  {
    const bin = new Uint8Array(257); bin[0] = 66; /* B */ for (let i = 0; i < 256; i++) bin[i + 1] = i;
    const got = [];
    const cli = TCPServer.connect({ host: HOST, port: PORT }, {
      connect: (c) => c.write(bin),
      data: (c, b) => { for (const x of b) got.push(x); },
      close: () => {}});
    await P(500);
    cli.close();
    let ok = got.length === 257 && got[0] === 66;
    for (let i = 0; i < 256 && ok; i++) if (got[i + 1] !== i) ok = false;
    assert_true(ok, "tcp binary echo all 256 bytes intact, len=" + got.length);
  }
  // C: server replies then closes
  {
    const { events, cli } = await once("C", "Cignore", "close");
    if (cli) cli.close();
    assert_true(events.indexOf("data:bye\n") >= 0, "C: server reply seen");
    assert_true(events.indexOf("close") >= 0, "C: close event fires on peer FIN");
  }
  // H: half-close — echo (incl mode byte) then FIN, close still fires
  {
    const { events, cli } = await once("H", "Hping", "half");
    if (cli) cli.close();
    assert_true(events.indexOf("data:ping") >= 0, "H: echo received (mode byte consumed)");
    assert_true(events.indexOf("close") >= 0, "H: FIN surfaces as close");
  }
  // R: immediate reset — error or close, never hang
  {
    const { events, cli } = await once("R", "Rx", "reset");
    if (cli) cli.close();
    assert_true(events.some((e) => e.indexOf("err:") === 0) || events.indexOf("close") >= 0,
      "R: reset surfaces as err/close, got " + JSON.stringify(events));
  }
  // D: dribble — three separate data events (chunks are message boundaries at TCP level here)
  {
    const { events, cli } = await once("D", "Dx", "dribble");
    if (cli) cli.close();
    const parts = events.filter((e) => e.indexOf("data:part") === 0);
    assert_eq(parts.length, 3, "D: three dribbled chunks delivered, got " + JSON.stringify(events));
  }
  // Z: client sends nothing and closes its side; server sees EOF, closes;
  // client sees close with no data
  {
    const events = [];
    await new Promise((resolve) => {
      const cli = TCPServer.connect({ host: HOST, port: PORT }, {
        connect: (c) => { events.push("connect"); if (c) c.close(); },
        data: (c, b) => events.push("data:" + bytesToStr(b)),
        close: () => { events.push("close"); resolve(); }});
      setTimeout(resolve, 3000);
    });
    assert_true(events.indexOf("close") >= 0, "Z: close fires, got " + JSON.stringify(events));
    assert_true(!events.some((e) => e.indexOf("data:") === 0), "Z: no data events");
  }
  // connect refused (port 1 is privileged+closed: use a closed high port via udp socket trick)
  {
    const u = new UDPSocket({ port: 0, host: HOST });
    u.start({ message: () => {} });
    const closedPort = u.port; // bound (UDP) => no TCP listener on that port
    let refused = null;
    await new Promise((resolve) => {
      const c = TCPServer.connect({ host: HOST, port: closedPort }, {
        connect: (cc, err) => { refused = err || "connected?!"; if (cc) cc.close(); c.close(); resolve(); }});
      setTimeout(() => { try { c.close(); } catch (e) {} resolve(); }, 2000);
    });
    u.close();
    assert_eq(refused, "Connection refused", "connect to closed port reports Connection refused, got " + refused);
  }
  summary("net.tcp");
  // keep every resource released: probes must terminate
})();
