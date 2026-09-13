// h.js — portable micro test harness for black-box probes.
// Works identically on dynajs (has `print`) and node (console.log).
// Probe contract: emit per-failure FAIL lines, then a SUMMARY line and
// RESULT PASS / RESULT FAIL. On any failure the probe throws uncaught at
// the end so the process exit code is nonzero on BOTH engines.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v);
  if (t === "number") { if (v === 0 && 1 / v < 0) return "-0"; return String(v); }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  try { return String(v); } catch (e) { return "[unprintable]"; }
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __fail++;
  __out("FAIL assert " + msg + " cond=" + __show(cond));
}
function assert_true(v, msg) {
  if (v === true) { __pass++; return; }
  __fail++;
  __out("FAIL assert_true " + msg + " got=" + __show(v));
}
function assert_eq(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (a === e && typeof actual === typeof expected) { __pass++; return; }
  __fail++;
  __out("FAIL assert_eq " + msg + " got=" + a + " want=" + e);
}
function assert_ne(actual, expected, msg) {
  var a = __show(actual), e = __show(expected);
  if (!(a === e && typeof actual === typeof expected)) { __pass++; return; }
  __fail++;
  __out("FAIL assert_ne " + msg + " both=" + a);
}
function assert_throws(fn, kind, msg) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __fail++; __out("FAIL assert_throws " + msg + " no-throw"); return; }
  var name = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (kind && name !== kind) {
    __fail++; __out("FAIL assert_throws " + msg + " threw=" + name + " want=" + kind);
    return;
  }
  __pass++;
}

// ---- normalized case comparison (used by generated probes) ----
// __norm maps an arbitrary result value into a small JSON-able object so
// baked expectations (produced from node at generation time) can be deep
// compared against what the engine under test returns.
//  - strings <=512 units: {t:"s",v:str}; longer: {t:"S",len,fnv,head,tail,mid}
//  - numbers: {t:"n",v:String-form, "-0" kept distinct}
//  - undefined/null/boolean/function
//  - objects/arrays: JSON string <=512 chars, else digest of the JSON text
//  - errors are normalized by the caller of case_eq (name only; message text
//    is implementation-defined per spec and asserted separately when wanted)
function __fnv1a(str) {
  var h = 0x811c9dc5 | 0;
  for (var i = 0; i < str.length; i++) {
    h = h ^ str.charCodeAt(i);
    h = Math.imul(h, 16777619) | 0;
  }
  return (h >>> 0);
}
function __slice3(s) {
  var L = s.length;
  return [s.slice(0, 64), s.slice(L - 64), s.slice((L >> 1) - 32, (L >> 1) + 32)];
}
function __norm(r) {
  var t = typeof r;
  if (t === "string") {
    if (r.length <= 512) return { t: "s", v: r };
    var p = __slice3(r);
    return { t: "S", len: r.length, fnv: __fnv1a(r), h: p[0], a: p[1], m: p[2] };
  }
  if (t === "number") {
    if (r === 0 && 1 / r < 0) return { t: "n", v: "-0" };
    return { t: "n", v: String(r) };
  }
  if (r === undefined) return { t: "u" };
  if (r === null) return { t: "z" };
  if (t === "boolean") return { t: "b", v: String(r) };
  if (t === "function") return { t: "f" };
  var j;
  try { j = JSON.stringify(r); } catch (e) { return { t: "jerr", m: String(e && e.name) }; }
  if (j === undefined) return { t: "u" };
  if (j.length <= 512) return { t: "j", v: j };
  var p2 = __slice3(j);
  return { t: "J", len: j.length, fnv: __fnv1a(j), h: p2[0], a: p2[1], m: p2[2] };
}
function __deepEq(a, b) {
  if (a === b) return true;
  if (a === null || b === null || a === undefined || b === undefined) return false;
  if (typeof a !== "object" || typeof b !== "object") return false;
  var ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (var i = 0; i < ka.length; i++) {
    if (!Object.prototype.hasOwnProperty.call(b, ka[i])) return false;
    if (!__deepEq(a[ka[i]], b[ka[i]])) return false;
  }
  return true;
}
function case_eq(id, want, fn) {
  var got;
  try { got = __norm(fn()); }
  catch (e) {
    var n = (e && typeof e.name === "string") ? e.name : "Error";
    got = { t: "e", n: n };
  }
  var w = (want && want.__raw) ? want.v : want;
  if (__deepEq(got, w)) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(w));
}
// error-name-level case (message text is spec-implementation-defined)
function case_err(id, wantName, fn) {
  var threw = null;
  try { fn(); } catch (e) { threw = e; }
  if (threw === null) { __fail++; __out("FAIL case " + id + " no-throw want=" + wantName); return; }
  var n = (threw && typeof threw.name === "string") ? threw.name : "?";
  if (n === wantName) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " threw=" + n + " want=" + wantName);
}
// assert without baked expectation (engine-under-test invariants only)
function case_true(id, fn) {
  var v;
  try { v = fn(); } catch (e) { __fail++; __out("FAIL case " + id + " threw=" + __show(e)); return; }
  if (v === true) { __pass++; return; }
  __fail++;
  __out("FAIL case " + id + " got=" + __show(v));
}

function summary(tag) {
  __out("SUMMARY " + tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + tag);
  }
  __out("RESULT PASS");
}
