import { Exec, Which, env, cwd} from "dyna:sys";
import { } from "dyna:file";
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

// ---- generated probe sys/exec ----

// stdout/stderr/code
const r1 = Exec("/bin/sh", ["-c", "echo out; echo err >&2"]);
assert_eq(r1.code, 0, "exit code 0");
assert_eq(r1.stdout, "out\n", "stdout exact");
assert_eq(r1.stderr, "err\n", "stderr exact");
assert_eq(r1.timedOut, false, "timedOut false");
assert_eq(r1.signal, null, "signal null on clean exit");

// exit code passthrough
const r2 = Exec("/bin/sh", ["-c", "exit 7"]);
assert_eq(r2.code, 7, "exit code passthrough");

// signal: code null, signal named
const r3 = Exec("/bin/sh", ["-c", "kill -TERM $$"]);
assert_eq(r3.code, null, "code null when signal killed the child");
assert_eq(r3.signal, "SIGTERM", "signal names the killer");

// argv fidelity: quoting matrix through argv (no shell anywhere)
const q = Exec("/usr/bin/printf", ["%s\\n", "a b", 'c"d', "e'f", "", "--x=1", "-", "--", "-y"]);
const parts = q.stdout.slice(0, -1).split("\n");
assert_eq(parts.length, 8, "8 argv elements echo as 8 lines, got " + parts.length);
assert_eq(parts[0], "a b", "embedded space survives argv");
assert_eq(parts[1], 'c"d', "embedded double quote survives");
assert_eq(parts[2], "e'f", "embedded single quote survives");
assert_eq(parts[3], "", "empty arg survives");
assert_eq(parts[4], "--x=1", "flag-looking arg survives");
assert_eq(parts[6], "--", "double-dash passes through (no shell)");

// input to stdin
const r4 = Exec("cat", [], { input: "hello-stdin" });
assert_eq(r4.stdout, "hello-stdin", "stdin input forwarded");

// encoding bytes
const r5 = Exec("/bin/sh", ["-c", "printf \'\\377\\200\\001\'"], { encoding: "bytes" });
assert_true(r5.stdout instanceof Uint8Array, "encoding bytes -> Uint8Array");
assert_eq(r5.stdout.length, 3, "binary bytes length 3");
assert_eq(r5.stdout[0], 255, "byte 0xFF survives");

// env replacement: child env EXACTLY the object
const r6 = Exec("/usr/bin/env", [], { env: { FOO: "bar", BAZ: "qux" } });
const lines = r6.stdout.trim().split("\n").sort();
assert_eq(lines.join("|"), "BAZ=qux|FOO=bar", "env replaces (not augments), got " + lines.join("|"));

// cwd option
const here = cwd();
const r7 = Exec("/bin/pwd", [], { cwd: "/private/tmp" });
assert_eq(r7.stdout.trim(), "/private/tmp", "cwd option sets child dir");

// timeout: SIGTERM then SIGKILL
const t0 = Date.now();
const r8 = Exec("/bin/sh", ["-c", "sleep 30"], { timeoutMs: 200 });
assert_eq(r8.timedOut, true, "timeout flags timedOut");
assert_eq(r8.signal, "SIGTERM", "timeout SIGTERMs the child");
assert_true(Date.now() - t0 < 5000, "timeout is prompt");

// maxBuffer: throw, not truncate
let mbErr = null;
try {
  Exec("/bin/sh", ["-c", "yes abcd | head -c 100000"], { maxBuffer: 1024 });
} catch (e) { mbErr = e; }
assert_true(mbErr !== null, "maxBuffer exceeded throws");

// missing command: clear error, not exit 127
let missErr = null;
try { Exec("definitely-not-a-command-xyz", []); } catch (e) { missErr = e; }
assert_true(missErr !== null, "missing command throws");

// PATH resolution happens against child's PATH when env replaced
const w = Which("sh");
assert_true(w !== null && w.startsWith("/"), "Which resolves against PATH: " + w);
assert_eq(Which("definitely-not-a-command-xyz"), null, "Which returns null when not found");
summary("sys.exec");
