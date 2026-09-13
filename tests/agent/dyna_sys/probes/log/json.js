import { Logger} from "dyna:log";
import { Path, readFile, makeDir, removeAll} from "dyna:file";
import { pid as getPid} from "dyna:sys";
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

// ---- generated probe log/json ----

const D = new Path(Path.cwd(), "scratch", "log_json-" + getPid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const dest = new Path(D, "app.log");

const log = new Logger({ level: "trace", name: "app", dest: String(dest), timestamp: false });
log.trace("t"); log.debug("d"); log.info("i"); log.warn("w"); log.error("e"); log.fatal("f");
log.flush();

const lines = readFile(dest).trim().split("\n");
assert_eq(lines.length, 6, "all six levels emitted at level=trace");
const parsed = lines.map((l) => JSON.parse(l));
assert_eq(parsed.map((p) => p.level).join(","), "trace,debug,info,warn,error,fatal", "level order");
assert_eq(parsed[0].msg, "t", "msg field");
assert_eq(parsed[0].name, "app", "name field");

// level filtering: warn threshold drops info and below
const dest2 = new Path(D, "warn.log");
const lg2 = new Logger({ level: "warn", name: "app", dest: String(dest2), timestamp: false });
lg2.info("nope"); lg2.warn("yes"); lg2.error("yes2");
lg2.flush();
const l2 = readFile(dest2).trim().split("\n");
assert_eq(l2.length, 2, "level filter drops below-threshold lines");
assert_eq(JSON.parse(l2[0]).level, "warn", "first surviving level");

// silent drops everything
const dest3 = new Path(D, "silent.log");
const lg3 = new Logger({ level: "silent", name: "a", dest: String(dest3), timestamp: false });
lg3.error("swallowed");
lg3.flush();
assert_eq(readFile(dest3).trim(), "", "silent emits nothing");

// enabled() + level setter
assert_eq(lg2.enabled("error"), true, "enabled(error) at warn");
assert_eq(lg2.enabled("info"), false, "enabled(info) false at warn");
assert_throws(() => lg2.enabled("bogus"), null, "enabled(unknown) throws");
assert_throws(() => { lg2.level = "bogus"; }, null, "level setter rejects unknown");
lg2.level = "debug";
assert_eq(lg2.level, "debug", "level setter + getter");

// timestamp shapes
const destE = new Path(D, "epoch.log");
const le = new Logger({ level: "info", dest: String(destE) });  // default epoch
le.info("x");
le.flush();
const pe = JSON.parse(readFile(destE).trim());
assert_eq(typeof pe.time, "number", "epoch timestamp is a number");
assert_true(pe.time > 1600000000000, "epoch ms plausible");

// base fields + pid + hostname + child fields; caller keys cannot forge frame keys
const destB = new Path(D, "base.log");
const lb = new Logger({ level: "info", dest: String(destB), timestamp: false,
                        base: { env: "dev" }, pid: true, hostname: true });
lb.warn({ msg: "forged", level: "fatal" }, "real message");
const child = lb.child({ route: "/api" }, { level: "error" });
child.error({ id: 7 }, "child msg");
child.info("dropped-by-child-level");
lb.flush();
const bl = readFile(destB).trim().split("\n").map((l) => JSON.parse(l));
assert_eq(bl.length, 2, "child level override filtered the info line");
assert_eq(bl[0].msg, "real message", "caller cannot forge msg");
assert_eq(bl[0].level, "warn", "caller cannot forge level");
assert_eq(bl[0].env, "dev", "base field present");
assert_eq(typeof bl[0].pid, "number", "pid added");
assert_eq(typeof bl[0].hostname, "string", "hostname added");
assert_eq(bl[1].route, "/api", "child field present");

// message shapes: (fields, msg), (err, msg), (err, fields, msg)
const destS = new Path(D, "shapes.log");
const ls = new Logger({ level: "info", dest: String(destS), timestamp: false });
const boom = new Error("boom");
boom.code = "EBOOM";
ls.info({ k: 1 }, "fields then msg");
ls.error(boom, "err then msg");
ls.error(boom, { k: 2 }, "err, fields, msg");
ls.flush();
const sl = readFile(destS).trim().split("\n").map((l) => JSON.parse(l));
assert_eq(sl[0].k, 1, "(fields, msg) shape");
assert_eq(sl[1].err.message, "boom", "(err, msg) serializes {type,message,stack}");
assert_eq(sl[1].err.code, "EBOOM", "extra enumerable props carried");
assert_eq(sl[2].k, 2, "(err, fields, msg) shape");
assert_true(typeof sl[1].err.stack === "string", "stack serialized");

// unknown constructor options refuse rather than silently misbehave
assert_throws(() => new Logger({ timestamp: "bogus" }), null, "unknown timestamp string throws");
assert_throws(() => new Logger({ level: "bogus" }), null, "unknown level throws");
{ const b0 = new Logger({ level: "info", dest: String(new Path(D, "buf0.log")), timestamp: false, buffer: 0 }); b0.info("ok"); }
assert_eq(JSON.parse(readFile(new Path(D, "buf0.log")).trim()).msg, "ok", "buffer:0 is legal (unbuffered)");

removeAll(D);
summary("log.json");
