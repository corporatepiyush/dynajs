// h.js — portable assert harness for the DynaJS constprop black-box review.
// Valid under ./dynajs (script or -m module) and node (file or --input-type=module),
// in sloppy, strict, and module contexts. var/function only, no engine-specifics.
var __T = { pass: 0, fail: 0 };
function __fmt(v) {
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  var t = typeof v;
  if (t === "number") {
    if (v !== v) return "NaN";
    if (v === Infinity) return "Infinity";
    if (v === -Infinity) return "-Infinity";
    if (v === 0 && 1 / v === -Infinity) return "-0";
    return String(v);
  }
  if (t === "string") return JSON.stringify(v);
  if (t === "bigint") return String(v) + "n";
  if (t === "symbol") return v.toString();
  if (t === "function") return "function";
  if (t === "boolean") return String(v);
  return "object";
}
function __etag(e) {
  try {
    if (e instanceof TypeError) return "TypeError";
    if (e instanceof RangeError) return "RangeError";
    if (e instanceof ReferenceError) return "ReferenceError";
    if (e instanceof SyntaxError) return "SyntaxError";
    if (e instanceof EvalError) return "EvalError";
    if (e instanceof Error) return "Error";
    return "non-Error throw";
  } catch (e2) { return "unknown"; }
}
function __ok(id) { __T.pass++; }
function __bad(id, msg) { __T.fail++; console.log("FAIL " + id + " " + msg); }
function __chk(id, got, want) {
  if (got === want) __ok(id); else __bad(id, "got=" + got + " want=" + want);
}
function __chkThrow(id, gotTag, wantTag) {
  if (gotTag === wantTag) __ok(id); else __bad(id, "threw=" + gotTag + " want=" + wantTag);
}
function __no_throw(id) { __bad(id, "no-throw (expected a throw)"); }
function __unexp_throw(id, tag) { __bad(id, "unexpected throw=" + tag); }
function __summary() {
  console.log("SUMMARY pass=" + __T.pass + " fail=" + __T.fail);
  if (__T.fail > 0) { throw new Error("HARNESS: " + __T.fail + " failing asserts"); }
}
function __cap(pid, sid, val) { console.log("V " + pid + " " + sid + " " + val); }
function __capT(pid, sid, e) { console.log("T " + pid + " " + sid + " " + (e && e.name ? e.name : "unknown")); }
// friendly API (per review contract): assert_eq / assert_throws / test
function assert_eq(actual, expected, name) { __chk(String(name), __fmt(actual), __fmt(expected)); }
function assert_throws(fn, ctorName, name) {
  try { fn(); __no_throw(String(name)); } catch (e) { __chkThrow(String(name), __etag(e), ctorName); }
}
function test(name, fn) {
  try { fn(); } catch (e) { __bad("test:" + name, "threw=" + __etag(e)); }
}
