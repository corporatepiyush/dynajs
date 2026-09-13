// h.js -- portable assert harness for DynaJS black-box review (defects_bb).
// Strict-mode safe so it can be embedded in ES module probes too.
// Provides: test / assert / assert_eq / assert_ne / assert_throws / assert_async,
// event log, recorder hook (__REC__ set by recpreload.js under node at bake time),
// __finish__() => prints HARNESS summary and exits nonzero on failure.
var __h_pass = 0;
var __h_fail = 0;
var __h_failed = [];
var __h_obs = [];   // observed value log (x1 value semantics etc.)
var __h_log = [];   // event trace log (x2/x3/x5/x6 ordering)
function __h_str(v) {
  if (typeof v === "string") { return v; }
  try { return JSON.stringify(v); } catch (e) { return String(v); }
}
function __h_deep(a, b) {
  if (a === b) { return true; }
  if (typeof a !== typeof b) { return false; }
  if (a === null || b === null) { return false; }
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b)) { return false; }
    if (a.length !== b.length) { return false; }
    for (var i = 0; i < a.length; i++) { if (!__h_deep(a[i], b[i])) { return false; } }
    return true;
  }
  if (typeof a === "object") {
    var ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) { return false; }
    for (var j = 0; j < ka.length; j++) {
      if (!Object.prototype.hasOwnProperty.call(b, ka[j])) { return false; }
      if (!__h_deep(a[ka[j]], b[ka[j]])) { return false; }
    }
    return true;
  }
  return false;
}
function assert(cond, msg) {
  if (!cond) { throw new Error("assert" + (msg ? " [" + msg + "]" : "")); }
}
function assert_eq(actual, expected, msg) {
  if (!__h_deep(actual, expected)) {
    throw new Error("assert_eq" + (msg ? " [" + msg + "]" : "") +
      " got=" + __h_str(actual) + " want=" + __h_str(expected));
  }
}
function assert_ne(actual, expected, msg) {
  if (__h_deep(actual, expected)) {
    throw new Error("assert_ne" + (msg ? " [" + msg + "]" : "") +
      " both=" + __h_str(actual));
  }
}
function assert_throws(fn, errName, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) {
    throw new Error("assert_throws: did not throw" + (msg ? " [" + msg + "]" : ""));
  }
  if (errName) {
    var ok = false;
    try { ok = threw instanceof errName; } catch (e) { ok = false; }
    if (!ok && threw.constructor && threw.constructor.name === errName.name) { ok = true; }
    if (!ok) {
      throw new Error("assert_throws: wrong error " + (threw && threw.constructor ? threw.constructor.name : String(threw)) +
        (msg ? " [" + msg + "]" : ""));
    }
  }
  return threw;
}
function assert_async(promiseOrFn, expected, msg) {
  return Promise.resolve().then(promiseOrFn).then(function (v) {
    assert_eq(v, expected, msg);
    return v;
  });
}
function test(name, fn) {
  try { fn(); __h_pass += 1; } catch (e) {
    __h_fail += 1;
    __h_failed.push(__h_name(name) + ": " + (e && e.message ? e.message : String(e)));
  }
}
function __h_name(n) { return n === undefined || n === null ? "assert" : String(n); }
function __h_logf(name, fn) { // logging variant: returns fn's value, logs nothing itself
  return fn();
}
function __h_exit__(code) {
  // node: graceful exit so buffered stdout fully flushes (process.exit
  // truncates pending async pipe writes). dynajs: no process -> std.exit.
  if (typeof process !== "undefined" && process) {
    try { process.exitCode = code; return; } catch (e) { }
  }
  if (typeof std !== "undefined" && std && typeof std.exit === "function") { std.exit(code); }
}
function __finish__() {
  var i;
  console.log("HARNESS pass=" + __h_pass + " fail=" + __h_fail);
  for (i = 0; i < __h_failed.length; i++) { console.log("FAILED: " + __h_failed[i]); }
  __h_exit__(__h_fail > 0 ? 1 : 0);
}
function __rec_out__() {
  console.log("RECDATA:" + JSON.stringify({ obs: __h_obs, log: __h_log, pass: __h_pass, fail: __h_fail }));
  __h_exit__(0);
}
// Serialize a binding's value for x1 value-semantics observations.
function __SER__(v) {
  if (typeof v === "function") {
    try { var r = v(); return "fn:" + __h_str(r); } catch (e1) {
      try { var o = new v(); return "cls:" + __h_str(o && o.tag); } catch (e2) {
        return "fnX:" + (e1 && e1.constructor && e1.constructor.name);
      }
    }
  }
  return typeof v + ":" + __h_str(v);
}
function __OBS__(s) { __h_obs.push(s); }
function LOG(s) { __h_log.push(String(s)); }
// Publish eval-reachable helpers as true globals: (0,eval) / new Function
// bodies evaluate in the global scope, and node CommonJS modules scope
// top-level declarations away from it (dynajs scripts do not).
var __h_G = (typeof globalThis !== "undefined") ? globalThis : (function(){ return this; })();
if (__h_G) {
  __h_G.__OBS__ = __OBS__;
  __h_G.__SER__ = __SER__;
  __h_G.LOG = LOG;
}
