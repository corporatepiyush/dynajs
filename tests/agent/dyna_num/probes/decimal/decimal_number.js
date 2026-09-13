// GENERATED probe (decimal_number) -- do not edit; run materialize.sh
import { Decimal } from "dyna:decimal";
var __TAG = "decimal_number";
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
var F = [
0.1,0.2,0.3,1.5,2.675,-0.1,1e-07,1.5e-07,1e+21,1e-21,123.456,1000000000000000.0,9007199254740991,9007199254740992,5e-324,1.7976931348623157e+308,2.2250738585072014e-308,-42.42,0.30000000000000004,0.3333333333333333,1000000.0,1234567890000000.0,-1e-15,7,-0.0
];
var GOLD = ["0.1", "0.2", "0.3", "1.5", "2.675", "-0.1", "0.0000001", "0.00000015", "1000000000000000000000", "0.000000000000000000001", "123.456", "1000000000000000", "9007199254740991", "9007199254740992", "0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000005", "179769313486231570000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", "0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000022250738585072014", "-42.42", "0.30000000000000004", "0.3333333333333333", "1000000", "1234567890000000", "-0.000000000000001", "7", "0"];

for (var i = 0; i < F.length; i++) {
  assert_eq(new Decimal(F[i]).toString(), GOLD[i], "ctor(number)#" + i);
  var tn = new Decimal(F[i]).toNumber();
  assert_eq(tn, F[i] === 0 ? 0 : F[i], "toNumber roundtrip#" + i);
}

var TN = [
["123456789012345678901234567890",1.2345678901234568e+29],
["0.1",0.1],
["1e308",1e+308],
["1.7976931348623157e308",1.7976931348623157e+308],
["1e309",Infinity],
["-1e309",-Infinity],
["4.9406564584124654e-324",5e-324],
["1e-400",0],
["123.4567890123456789012345678901234567890",123.45678901234568],
["1e6143",Infinity]
];

for (var i = 0; i < TN.length; i++) {
  var got;
  try { got = new Decimal(TN[i][0]).toNumber(); }
  catch (e) { __fail++; __out("FAIL toNumber(" + TN[i][0] + ") THREW " + e.name); continue; }
  assert_eq(got, TN[i][1], "toNumber(" + TN[i][0] + ")");
}


var x = new Decimal(1);
var a1 = x.div(3, { precision: 5 }).toString();
assert_eq(a1, "0.33333", "leak: div prec5");
var a2 = x.div(3).toString();
assert_eq(a2, "0.3333333333333333333333333333333333", "leak: div default still 34");
var a3 = x.div(3, { precision: 50 }).toString();
assert_eq(a3.length, 52, "leak: div prec50 digit count"); // 0. + 50 digits
assert_eq(x.div(3, { precision: 5 }).toString(), "0.33333", "leak: repeat prec5");
assert_eq(x.div(3).toString(), "0.3333333333333333333333333333333333", "leak: default after prec5 again");

assert_throws(function () { new Decimal(1).div(3, {precision:0}).toString(); }, "RangeError", "opts {precision:0}");
assert_throws(function () { new Decimal(1).div(3, {precision:-1}).toString(); }, "RangeError", "opts {precision:-1}");
assert_throws(function () { new Decimal(1).div(3, {precision:5001}).toString(); }, "RangeError", "opts {precision:5001}");
try { new Decimal(1).div(3, {precision:5000}).toString(); } catch (e) { __fail++; __out("FAIL opts " + "{precision:5000}" + " threw " + e.name); }
assert_throws(function () { new Decimal(1).div(3, {precision:34.5}).toString(); }, "TypeError", "opts {precision:34.5}");
try { new Decimal(1).div(3, {precision:"34"}).toString(); } catch (e) { __fail++; __out("FAIL opts " + "{precision:\"34\"}" + " threw " + e.name); }
assert_throws(function () { new Decimal(1).div(3, {rounding:"bogus"}).toString(); }, "RangeError", "opts {rounding:\"bogus\"}");
assert_throws(function () { new Decimal(1).div(3, {rounding:42}).toString(); }, "RangeError", "opts {rounding:42}");
assert_throws(function () { new Decimal(1).div(3, {rounding:"halFeven"}).toString(); }, "RangeError", "opts {rounding:\"halFeven\"}");
assert_throws(function () { new Decimal(1).div(3, "notanobject").toString(); }, "RangeError", "opts \"notanobject\"");
try { new Decimal(1).div(3, {rounding:"halfEven"}).toString(); } catch (e) { __fail++; __out("FAIL opts " + "{rounding:\"halfEven\"}" + " threw " + e.name); }
assert_throws(function () { new Decimal(1).div(0); }, "RangeError", "divzero new Decimal(1).div(0)");
assert_throws(function () { new Decimal(1).div("0"); }, "RangeError", "divzero new Decimal(1).div(\"0\")");
assert_throws(function () { new Decimal(0).div(0); }, "RangeError", "divzero new Decimal(0).div(0)");
assert_throws(function () { new Decimal("5").mod(0); }, "RangeError", "divzero new Decimal(\"5\").mod(0)");
assert_throws(function () { new Decimal("1e2000000000").pow(2); }, "RangeError", "pow exp overflow");

for (var i = 0; i < F.length; i++) {
  var d = new Decimal(F[i]);
  assert_eq(d.abs().toString(), new Decimal(GOLD[i]).abs().toString(), "abs#" + i);
  var negs = d.neg().toString();
  if (d.toString() !== "0")
    assert_eq(negs.charAt(0) === "-" ? negs.slice(1) : "-" + negs,
              d.toString(), "neg#" + i);
  else
    assert_eq(negs, "0", "neg#" + i);
}

summary(__TAG); // 132 cases
