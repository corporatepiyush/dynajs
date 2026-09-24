// GENERATED probe (mathx_expr) -- do not edit; run materialize.sh
import { Expression } from "dyna:mathx";
var __TAG = "mathx_expr";
// h.js — portable micro test harness for black-box numeric probes.

// Works identically on dynajs (has `print`) and node (console.log).
// Probe contract: emit per-failure FAIL lines, then a SUMMARY line and
// RESULT PASS / RESULT FAIL. On any failure the probe throws uncaught at
// the end so the process exit code is nonzero.
//
// Numeric additions over the strnum_bb harness:
//   assert_close(a, e, rel, abs, msg)  |a-e| <= max(abs, rel*|e|); NaN==NaN passes
//   assert_nan(v, msg)                 value must be NaN
//   f32bits(x)                         f32 round x, return the 32-bit pattern
//   assert_f32eq(x, bits, msg)         x must be the exact f32 bit pattern `bits`
//   assert_arr_eq(a, b, msg)           exact element equality via toString
//   assert_arr_close(a, e, rel, abs, msg)  elementwise assert_close
// The PRIMARY oracle for every dyna:* numeric probe is the python golden
// (decimal/numpy) baked into the probe at generation time — node cannot load
// dyna:* modules, so it is never the oracle here.
var __out = (typeof print === "function") ? function (s) { print(s); }
                                          : function (s) { console.log(s); };
var __pass = 0, __fail = 0;

function __show(v) {
  var t = typeof v;
  if (t === "string") return JSON.stringify(v);
  if (t === "number") {
    if (v !== v) return "NaN";
    if (v === 0 && 1 / v < 0) return "-0";
    if (v === Infinity) return "Infinity";
    if (v === -Infinity) return "-Infinity";
    return String(v);
  }
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (t === "boolean") return String(v);
  if (t === "function") return "fn";
  if (typeof v === "object" && v && typeof v.length === "number" && v.BYTES_PER_ELEMENT) {
    var parts = [];
    for (var i = 0; i < Math.min(v.length, 8); i++) parts.push(__show(v[i]));
    return "[" + parts.join(",") + (v.length > 8 ? ",...(" + v.length + ")" : "") + "]";
  }
  try { return String(v); } catch (e) { return "[unprintable]"; }
}

function assert(cond, msg) {
  if (cond) { __pass++; return; }
  __fail++;
  __out("FAIL assert " + msg);
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
// |a-e| <= max(abs, rel*|e|). Two NaNs count as equal (the generator only
// bakes NaN where NaN is the intended answer); exactly one NaN fails.
function assert_close(actual, expected, rel, abs, msg) {
  var a = +actual, e = +expected;
  if (a !== a && e !== e) { __pass++; return; }
  if (a !== a || e !== e) {
    __fail++;
    __out("FAIL assert_close " + msg + " got=" + __show(a) + " want=" + __show(e));
    return;
  }
  var d = Math.abs(a - e), lim = Math.max(abs, rel * Math.abs(e));
  if (e === Infinity || e === -Infinity) { if (a === e) { __pass++; return; } }
  if (d <= lim) { __pass++; return; }
  __fail++;
  __out("FAIL assert_close " + msg + " got=" + __show(a) + " want=" + __show(e) +
        " |d|=" + d + " lim=" + lim);
}
function assert_nan(v, msg) {
  if (v !== v) { __pass++; return; }
  __fail++;
  __out("FAIL assert_nan " + msg + " got=" + __show(v));
}
var __f32 = new Float32Array(1), __u32 = new Uint32Array(__f32.buffer);
function f32bits(x) { __f32[0] = x; return __u32[0]; }
function bitsf32(b) { __u32[0] = b >>> 0; return __f32[0]; }
function assert_f32eq(x, bits, msg) {
  var got = f32bits(x) >>> 0, want = bits >>> 0;
  if (got === want) { __pass++; return; }
  __fail++;
  __out("FAIL assert_f32eq " + msg + " got bits=0x" + got.toString(16) +
        " (" + __show(bitsf32(got)) + ") want bits=0x" + want.toString(16) +
        " (" + __show(bitsf32(want)) + ")");
}
// 32-bit pattern of a float64 (for -0 / NaN payload style checks).
var __f64 = new Float64Array(1), __u64 = new Uint32Array(__f64.buffer);
function f64bits(x) { __f64[0] = x; return [__u64[0], __u64[1]]; }
function assert_arr_eq(a, b, msg) {
  if (a.length !== b.length) {
    __fail++; __out("FAIL assert_arr_eq " + msg + " len " + a.length + " != " + b.length);
    return;
  }
  for (var i = 0; i < a.length; i++) {
    var x = a[i], y = b[i];
    var ok = (x === y) || (x !== x && y !== y) ||
             (typeof x === "string" && x === y);
    if (!ok) {
      __fail++;
      __out("FAIL assert_arr_eq " + msg + " at [" + i + "] got=" + __show(x) +
            " want=" + __show(y));
      return;
    }
  }
  __pass++;
}
function assert_arr_close(a, e, rel, abs, msg) {
  if (a.length !== e.length) {
    __fail++; __out("FAIL assert_arr_close " + msg + " len " + a.length + " != " + e.length);
    return;
  }
  for (var i = 0; i < a.length; i++) {
    var x = +a[i], y = +e[i];
    if (x !== x && y !== y) continue;
    if (x !== x || y !== y || Math.abs(x - y) > Math.max(abs, rel * Math.abs(y))) {
      __fail++;
      __out("FAIL assert_arr_close " + msg + " at [" + i + "] got=" + __show(x) +
            " want=" + __show(y));
      return;
    }
  }
  __pass++;
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
function summary(tag) {
  __out("SUMMARY " + tag + " pass=" + __pass + " fail=" + __fail);
  if (__fail > 0) {
    __out("RESULT FAIL");
    throw new Error("PROBE FAILED: " + __fail + " failure(s) in " + tag);
  }
  __out("RESULT PASS");
}

function f64fromhex(w) {
  var u = new Uint32Array(2);
  u[0] = parseInt(w[0], 16); u[1] = parseInt(w[1], 16);
  return (new Float64Array(u.buffer))[0];
}
assert_close(new Expression("a * x^2 + b").eval({"a": 2, "x": 3, "b": 1}), 19.0, 1e-12, 1e-15, "expr a * x^2 + b");
assert_close(new Expression("2^3^2").eval({}), 512.0, 1e-12, 1e-15, "expr 2^3^2");
assert_close(new Expression("sin(x) + sqrt(y)").eval({"x": 0, "y": 16}), 4.0, 1e-12, 1e-15, "expr sin(x) + sqrt(y)");
assert_close(new Expression("1 + 2 * 3").eval({}), 7.0, 1e-12, 1e-15, "expr 1 + 2 * 3");
assert_close(new Expression("(1 + 2) * 3").eval({}), 9.0, 1e-12, 1e-15, "expr (1 + 2) * 3");
assert_close(new Expression("2 * pi").eval({}), 6.283185307179586, 1e-12, 1e-15, "expr 2 * pi");
assert_close(new Expression("-x^2").eval({"x": 3}), -9.0, 1e-12, 1e-15, "expr -x^2");
assert_close(new Expression("x / 0").eval({"x": 1}), Infinity, 1e-12, 1e-15, "expr x / 0");
assert_close(new Expression("abs(-5) + min(2,3) + max(2,3)").eval({}), 10.0, 1e-12, 1e-15, "expr abs(-5) + min(2,3) + max(2,3)");
var vv = new Expression("a * x^2 + b").variables();
assert_arr_eq(String(vv).split(",").sort(), ["a","b","x"].sort(), "variables");
assert_eq(new Expression("2^3^2").eval(), 512, "right-assoc ^");
assert_throws(function () { new Expression(""); }, "SyntaxError", "reject ");
assert_throws(function () { new Expression("1 +"); }, "SyntaxError", "reject 1 +");
assert_throws(function () { new Expression("* 3"); }, "SyntaxError", "reject * 3");
assert_throws(function () { new Expression("((1)"); }, "SyntaxError", "reject ((1)");
assert_throws(function () { new Expression("1 $ 2"); }, "SyntaxError", "reject 1 $ 2");
assert_throws(function () { new Expression("func("); }, "SyntaxError", "reject func(");
assert_throws(function () { new Expression(".."); }, "SyntaxError", "reject ..");
summary(__TAG); // 18 cases
