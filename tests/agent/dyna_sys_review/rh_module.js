// rh_module.js — INDEPENDENT assert harness (ES module) for the
// dyna_sys_review probes. Written fresh for this review lane. Output
// conventions match the repo's probe family: FAIL lines, then
// "SUMMARY <tag> pass=N fail=N", then "RESULT PASS"|"RESULT FAIL"; a failing
// probe throws so the process exit code is nonzero.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0, __tag = "review";
var __pending = 0, __doneCalled = false, __waitBudgetMs = 15000;

export function setTag(t) { __tag = t; }
export function passCount() { return __pass; }
export function failCount() { return __fail; }

export function ok(cond, msg) {
  if (cond) { __pass++; return true; }
  __fail++;
  __out("FAIL " + msg);
  return false;
}
export function eq(actual, expected, msg) {
  var a, e;
  try { a = JSON.stringify(actual); } catch (er) { a = String(actual); }
  try { e = JSON.stringify(expected); } catch (er) { e = String(expected); }
  if (a === e && typeof actual === typeof expected) { __pass++; return true; }
  __fail++;
  __out("FAIL " + msg + " got=" + a + " want=" + e);
  return false;
}
export function throws(fn, msg) {
  try { fn(); } catch (e) { __pass++; return e; }
  __fail++;
  __out("FAIL " + msg + " (no throw)");
  return undefined;
}
// track(promise, cb(err, value)) — count in-flight async checks
export function track(p, cb) {
  __pending++;
  p.then(function (v) { __pending--; if (cb) cb(null, v); },
         function (e) { __pending--; if (cb) cb(e, null); });  return p;
}
export function done() {
  __doneCalled = true;
  if (__pending === 0) { finish(); return; }
  var step = function () {
    if (__pending === 0) { finish(); return; }
    if (__waitBudgetMs <= 0) {
      __out("FAIL async-timeout " + __pending + " pending check(s)");
      __fail += __pending;
      __pending = 0;
      finish();
      return;
    }
    var chunk = __waitBudgetMs < 100 ? __waitBudgetMs : 100;
    __waitBudgetMs -= chunk;
    setTimeout(step, chunk);
  };
  setTimeout(step, 50);
}
function finish() {
  __out("SUMMARY " + __tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) { __out("RESULT FAIL"); throw new Error("PROBE FAILED: " + __fail); }
  __out("RESULT PASS");
}
