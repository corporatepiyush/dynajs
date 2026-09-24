// GENERATED probe (decimal_money) -- do not edit; run materialize.sh
import { Decimal, Money } from "dyna:decimal";
var __TAG = "decimal_money";
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
var Q = [
[new Money(0, "USD"),"toString","str","0.00",null],
[new Money(0, "USD"),"toDecimal","str","0",null],
[new Money(0, "USD"),"amount","int",0,null],
[new Money(0, "USD"),"currency","str","USD",null],
[new Money(0, "usd"),"toString","str","0.00",null],
[new Money(0, "usd"),"toDecimal","str","0",null],
[new Money(0, "usd"),"amount","int",0,null],
[new Money(0, "usd"),"currency","str","USD",null],
[new Money(0, "EUR"),"toString","str","0.00",null],
[new Money(0, "EUR"),"toDecimal","str","0",null],
[new Money(0, "EUR"),"amount","int",0,null],
[new Money(0, "EUR"),"currency","str","EUR",null],
[new Money(0, "JPY"),"toString","str","0",null],
[new Money(0, "JPY"),"toDecimal","str","0",null],
[new Money(0, "JPY"),"amount","int",0,null],
[new Money(0, "JPY"),"currency","str","JPY",null],
[new Money(0, "jpy"),"toString","str","0",null],
[new Money(0, "jpy"),"toDecimal","str","0",null],
[new Money(0, "jpy"),"amount","int",0,null],
[new Money(0, "jpy"),"currency","str","JPY",null],
[new Money(0, "BHD"),"toString","str","0.000",null],
[new Money(0, "BHD"),"toDecimal","str","0",null],
[new Money(0, "BHD"),"amount","int",0,null],
[new Money(0, "BHD"),"currency","str","BHD",null],
[new Money(0, "bhd"),"toString","str","0.000",null],
[new Money(0, "bhd"),"toDecimal","str","0",null],
[new Money(0, "bhd"),"amount","int",0,null],
[new Money(0, "bhd"),"currency","str","BHD",null],
[new Money(0, "KRW"),"toString","str","0",null],
[new Money(0, "KRW"),"toDecimal","str","0",null],
[new Money(0, "KRW"),"amount","int",0,null],
[new Money(0, "KRW"),"currency","str","KRW",null],
[new Money(0, "GBP"),"toString","str","0.00",null],
[new Money(0, "GBP"),"toDecimal","str","0",null],
[new Money(0, "GBP"),"amount","int",0,null],
[new Money(0, "GBP"),"currency","str","GBP",null],
[new Money(0, "CHF"),"toString","str","0.00",null],
[new Money(0, "CHF"),"toDecimal","str","0",null],
[new Money(0, "CHF"),"amount","int",0,null],
[new Money(0, "CHF"),"currency","str","CHF",null],
[new Money(0, "AAA"),"toString","str","0.00",null],
[new Money(0, "AAA"),"toDecimal","str","0",null],
[new Money(0, "AAA"),"amount","int",0,null],
[new Money(0, "AAA"),"currency","str","AAA",null],
[new Money(0, "ZZZ"),"toString","str","0.00",null],
[new Money(0, "ZZZ"),"toDecimal","str","0",null],
[new Money(0, "ZZZ"),"amount","int",0,null],
[new Money(0, "ZZZ"),"currency","str","ZZZ",null],
[new Money(0, "XXX"),"toString","str","0.00",null],
[new Money(0, "XXX"),"toDecimal","str","0",null],
[new Money(0, "XXX"),"amount","int",0,null],
[new Money(0, "XXX"),"currency","str","XXX",null],
[new Money(1, "USD"),"toString","str","0.01",null],
[new Money(1, "USD"),"toDecimal","str","0.01",null],
[new Money(1, "USD"),"amount","int",1,null],
[new Money(1, "USD"),"currency","str","USD",null],
[new Money(1, "usd"),"toString","str","0.01",null],
[new Money(1, "usd"),"toDecimal","str","0.01",null],
[new Money(1, "usd"),"amount","int",1,null],
[new Money(1, "usd"),"currency","str","USD",null],
[new Money(1, "EUR"),"toString","str","0.01",null],
[new Money(1, "EUR"),"toDecimal","str","0.01",null],
[new Money(1, "EUR"),"amount","int",1,null],
[new Money(1, "EUR"),"currency","str","EUR",null],
[new Money(1, "JPY"),"toString","str","1",null],
[new Money(1, "JPY"),"toDecimal","str","1",null],
[new Money(1, "JPY"),"amount","int",1,null],
[new Money(1, "JPY"),"currency","str","JPY",null],
[new Money(1, "jpy"),"toString","str","1",null],
[new Money(1, "jpy"),"toDecimal","str","1",null],
[new Money(1, "jpy"),"amount","int",1,null],
[new Money(1, "jpy"),"currency","str","JPY",null],
[new Money(1, "BHD"),"toString","str","0.001",null],
[new Money(1, "BHD"),"toDecimal","str","0.001",null],
[new Money(1, "BHD"),"amount","int",1,null],
[new Money(1, "BHD"),"currency","str","BHD",null],
[new Money(1, "bhd"),"toString","str","0.001",null],
[new Money(1, "bhd"),"toDecimal","str","0.001",null],
[new Money(1, "bhd"),"amount","int",1,null],
[new Money(1, "bhd"),"currency","str","BHD",null],
[new Money(1, "KRW"),"toString","str","1",null],
[new Money(1, "KRW"),"toDecimal","str","1",null],
[new Money(1, "KRW"),"amount","int",1,null],
[new Money(1, "KRW"),"currency","str","KRW",null],
[new Money(1, "GBP"),"toString","str","0.01",null],
[new Money(1, "GBP"),"toDecimal","str","0.01",null],
[new Money(1, "GBP"),"amount","int",1,null],
[new Money(1, "GBP"),"currency","str","GBP",null],
[new Money(1, "CHF"),"toString","str","0.01",null],
[new Money(1, "CHF"),"toDecimal","str","0.01",null],
[new Money(1, "CHF"),"amount","int",1,null],
[new Money(1, "CHF"),"currency","str","CHF",null],
[new Money(1, "AAA"),"toString","str","0.01",null],
[new Money(1, "AAA"),"toDecimal","str","0.01",null],
[new Money(1, "AAA"),"amount","int",1,null],
[new Money(1, "AAA"),"currency","str","AAA",null],
[new Money(1, "ZZZ"),"toString","str","0.01",null],
[new Money(1, "ZZZ"),"toDecimal","str","0.01",null],
[new Money(1, "ZZZ"),"amount","int",1,null],
[new Money(1, "ZZZ"),"currency","str","ZZZ",null],
[new Money(1, "XXX"),"toString","str","0.01",null],
[new Money(1, "XXX"),"toDecimal","str","0.01",null],
[new Money(1, "XXX"),"amount","int",1,null],
[new Money(1, "XXX"),"currency","str","XXX",null],
[new Money(-1, "USD"),"toString","str","-0.01",null],
[new Money(-1, "USD"),"toDecimal","str","-0.01",null],
[new Money(-1, "USD"),"amount","int",-1,null],
[new Money(-1, "USD"),"currency","str","USD",null],
[new Money(-1, "usd"),"toString","str","-0.01",null],
[new Money(-1, "usd"),"toDecimal","str","-0.01",null],
[new Money(-1, "usd"),"amount","int",-1,null],
[new Money(-1, "usd"),"currency","str","USD",null],
[new Money(-1, "EUR"),"toString","str","-0.01",null],
[new Money(-1, "EUR"),"toDecimal","str","-0.01",null],
[new Money(-1, "EUR"),"amount","int",-1,null],
[new Money(-1, "EUR"),"currency","str","EUR",null],
[new Money(-1, "JPY"),"toString","str","-1",null],
[new Money(-1, "JPY"),"toDecimal","str","-1",null],
[new Money(-1, "JPY"),"amount","int",-1,null],
[new Money(-1, "JPY"),"currency","str","JPY",null],
[new Money(-1, "jpy"),"toString","str","-1",null],
[new Money(-1, "jpy"),"toDecimal","str","-1",null],
[new Money(-1, "jpy"),"amount","int",-1,null],
[new Money(-1, "jpy"),"currency","str","JPY",null],
[new Money(-1, "BHD"),"toString","str","-0.001",null],
[new Money(-1, "BHD"),"toDecimal","str","-0.001",null],
[new Money(-1, "BHD"),"amount","int",-1,null],
[new Money(-1, "BHD"),"currency","str","BHD",null],
[new Money(-1, "bhd"),"toString","str","-0.001",null],
[new Money(-1, "bhd"),"toDecimal","str","-0.001",null],
[new Money(-1, "bhd"),"amount","int",-1,null],
[new Money(-1, "bhd"),"currency","str","BHD",null],
[new Money(-1, "KRW"),"toString","str","-1",null],
[new Money(-1, "KRW"),"toDecimal","str","-1",null],
[new Money(-1, "KRW"),"amount","int",-1,null],
[new Money(-1, "KRW"),"currency","str","KRW",null],
[new Money(-1, "GBP"),"toString","str","-0.01",null],
[new Money(-1, "GBP"),"toDecimal","str","-0.01",null],
[new Money(-1, "GBP"),"amount","int",-1,null],
[new Money(-1, "GBP"),"currency","str","GBP",null],
[new Money(-1, "CHF"),"toString","str","-0.01",null],
[new Money(-1, "CHF"),"toDecimal","str","-0.01",null],
[new Money(-1, "CHF"),"amount","int",-1,null],
[new Money(-1, "CHF"),"currency","str","CHF",null],
[new Money(-1, "AAA"),"toString","str","-0.01",null],
[new Money(-1, "AAA"),"toDecimal","str","-0.01",null],
[new Money(-1, "AAA"),"amount","int",-1,null],
[new Money(-1, "AAA"),"currency","str","AAA",null],
[new Money(-1, "ZZZ"),"toString","str","-0.01",null],
[new Money(-1, "ZZZ"),"toDecimal","str","-0.01",null],
[new Money(-1, "ZZZ"),"amount","int",-1,null],
[new Money(-1, "ZZZ"),"currency","str","ZZZ",null],
[new Money(-1, "XXX"),"toString","str","-0.01",null],
[new Money(-1, "XXX"),"toDecimal","str","-0.01",null],
[new Money(-1, "XXX"),"amount","int",-1,null],
[new Money(-1, "XXX"),"currency","str","XXX",null],
[new Money(5, "USD"),"toString","str","0.05",null],
[new Money(5, "USD"),"toDecimal","str","0.05",null],
[new Money(5, "USD"),"amount","int",5,null],
[new Money(5, "USD"),"currency","str","USD",null],
[new Money(5, "usd"),"toString","str","0.05",null],
[new Money(5, "usd"),"toDecimal","str","0.05",null],
[new Money(5, "usd"),"amount","int",5,null],
[new Money(5, "usd"),"currency","str","USD",null],
[new Money(5, "EUR"),"toString","str","0.05",null],
[new Money(5, "EUR"),"toDecimal","str","0.05",null],
[new Money(5, "EUR"),"amount","int",5,null],
[new Money(5, "EUR"),"currency","str","EUR",null],
[new Money(5, "JPY"),"toString","str","5",null],
[new Money(5, "JPY"),"toDecimal","str","5",null],
[new Money(5, "JPY"),"amount","int",5,null],
[new Money(5, "JPY"),"currency","str","JPY",null],
[new Money(5, "jpy"),"toString","str","5",null],
[new Money(5, "jpy"),"toDecimal","str","5",null],
[new Money(5, "jpy"),"amount","int",5,null],
[new Money(5, "jpy"),"currency","str","JPY",null],
[new Money(5, "BHD"),"toString","str","0.005",null],
[new Money(5, "BHD"),"toDecimal","str","0.005",null],
[new Money(5, "BHD"),"amount","int",5,null],
[new Money(5, "BHD"),"currency","str","BHD",null],
[new Money(5, "bhd"),"toString","str","0.005",null],
[new Money(5, "bhd"),"toDecimal","str","0.005",null],
[new Money(5, "bhd"),"amount","int",5,null],
[new Money(5, "bhd"),"currency","str","BHD",null],
[new Money(5, "KRW"),"toString","str","5",null],
[new Money(5, "KRW"),"toDecimal","str","5",null],
[new Money(5, "KRW"),"amount","int",5,null],
[new Money(5, "KRW"),"currency","str","KRW",null],
[new Money(5, "GBP"),"toString","str","0.05",null],
[new Money(5, "GBP"),"toDecimal","str","0.05",null],
[new Money(5, "GBP"),"amount","int",5,null],
[new Money(5, "GBP"),"currency","str","GBP",null],
[new Money(5, "CHF"),"toString","str","0.05",null],
[new Money(5, "CHF"),"toDecimal","str","0.05",null],
[new Money(5, "CHF"),"amount","int",5,null],
[new Money(5, "CHF"),"currency","str","CHF",null],
[new Money(5, "AAA"),"toString","str","0.05",null],
[new Money(5, "AAA"),"toDecimal","str","0.05",null],
[new Money(5, "AAA"),"amount","int",5,null],
[new Money(5, "AAA"),"currency","str","AAA",null],
[new Money(5, "ZZZ"),"toString","str","0.05",null],
[new Money(5, "ZZZ"),"toDecimal","str","0.05",null],
[new Money(5, "ZZZ"),"amount","int",5,null],
[new Money(5, "ZZZ"),"currency","str","ZZZ",null],
[new Money(5, "XXX"),"toString","str","0.05",null],
[new Money(5, "XXX"),"toDecimal","str","0.05",null],
[new Money(5, "XXX"),"amount","int",5,null],
[new Money(5, "XXX"),"currency","str","XXX",null],
[new Money(-5, "USD"),"toString","str","-0.05",null],
[new Money(-5, "USD"),"toDecimal","str","-0.05",null],
[new Money(-5, "USD"),"amount","int",-5,null],
[new Money(-5, "USD"),"currency","str","USD",null],
[new Money(-5, "usd"),"toString","str","-0.05",null],
[new Money(-5, "usd"),"toDecimal","str","-0.05",null],
[new Money(-5, "usd"),"amount","int",-5,null],
[new Money(-5, "usd"),"currency","str","USD",null],
[new Money(-5, "EUR"),"toString","str","-0.05",null],
[new Money(-5, "EUR"),"toDecimal","str","-0.05",null],
[new Money(-5, "EUR"),"amount","int",-5,null],
[new Money(-5, "EUR"),"currency","str","EUR",null],
[new Money(-5, "JPY"),"toString","str","-5",null],
[new Money(-5, "JPY"),"toDecimal","str","-5",null],
[new Money(-5, "JPY"),"amount","int",-5,null],
[new Money(-5, "JPY"),"currency","str","JPY",null],
[new Money(-5, "jpy"),"toString","str","-5",null],
[new Money(-5, "jpy"),"toDecimal","str","-5",null],
[new Money(-5, "jpy"),"amount","int",-5,null],
[new Money(-5, "jpy"),"currency","str","JPY",null],
[new Money(-5, "BHD"),"toString","str","-0.005",null],
[new Money(-5, "BHD"),"toDecimal","str","-0.005",null],
[new Money(-5, "BHD"),"amount","int",-5,null],
[new Money(-5, "BHD"),"currency","str","BHD",null],
[new Money(-5, "bhd"),"toString","str","-0.005",null],
[new Money(-5, "bhd"),"toDecimal","str","-0.005",null],
[new Money(-5, "bhd"),"amount","int",-5,null],
[new Money(-5, "bhd"),"currency","str","BHD",null],
[new Money(-5, "KRW"),"toString","str","-5",null],
[new Money(-5, "KRW"),"toDecimal","str","-5",null],
[new Money(-5, "KRW"),"amount","int",-5,null],
[new Money(-5, "KRW"),"currency","str","KRW",null],
[new Money(-5, "GBP"),"toString","str","-0.05",null],
[new Money(-5, "GBP"),"toDecimal","str","-0.05",null],
[new Money(-5, "GBP"),"amount","int",-5,null],
[new Money(-5, "GBP"),"currency","str","GBP",null],
[new Money(-5, "CHF"),"toString","str","-0.05",null],
[new Money(-5, "CHF"),"toDecimal","str","-0.05",null],
[new Money(-5, "CHF"),"amount","int",-5,null],
[new Money(-5, "CHF"),"currency","str","CHF",null],
[new Money(-5, "AAA"),"toString","str","-0.05",null],
[new Money(-5, "AAA"),"toDecimal","str","-0.05",null],
[new Money(-5, "AAA"),"amount","int",-5,null],
[new Money(-5, "AAA"),"currency","str","AAA",null],
[new Money(-5, "ZZZ"),"toString","str","-0.05",null],
[new Money(-5, "ZZZ"),"toDecimal","str","-0.05",null],
[new Money(-5, "ZZZ"),"amount","int",-5,null],
[new Money(-5, "ZZZ"),"currency","str","ZZZ",null],
[new Money(-5, "XXX"),"toString","str","-0.05",null],
[new Money(-5, "XXX"),"toDecimal","str","-0.05",null],
[new Money(-5, "XXX"),"amount","int",-5,null],
[new Money(-5, "XXX"),"currency","str","XXX",null],
[new Money(99, "USD"),"toString","str","0.99",null],
[new Money(99, "USD"),"toDecimal","str","0.99",null],
[new Money(99, "USD"),"amount","int",99,null],
[new Money(99, "USD"),"currency","str","USD",null],
[new Money(99, "usd"),"toString","str","0.99",null],
[new Money(99, "usd"),"toDecimal","str","0.99",null],
[new Money(99, "usd"),"amount","int",99,null],
[new Money(99, "usd"),"currency","str","USD",null],
[new Money(99, "EUR"),"toString","str","0.99",null],
[new Money(99, "EUR"),"toDecimal","str","0.99",null],
[new Money(99, "EUR"),"amount","int",99,null],
[new Money(99, "EUR"),"currency","str","EUR",null],
[new Money(99, "JPY"),"toString","str","99",null],
[new Money(99, "JPY"),"toDecimal","str","99",null],
[new Money(99, "JPY"),"amount","int",99,null],
[new Money(99, "JPY"),"currency","str","JPY",null],
[new Money(99, "jpy"),"toString","str","99",null],
[new Money(99, "jpy"),"toDecimal","str","99",null],
[new Money(99, "jpy"),"amount","int",99,null],
[new Money(99, "jpy"),"currency","str","JPY",null],
[new Money(99, "BHD"),"toString","str","0.099",null],
[new Money(99, "BHD"),"toDecimal","str","0.099",null],
[new Money(99, "BHD"),"amount","int",99,null],
[new Money(99, "BHD"),"currency","str","BHD",null],
[new Money(99, "bhd"),"toString","str","0.099",null],
[new Money(99, "bhd"),"toDecimal","str","0.099",null],
[new Money(99, "bhd"),"amount","int",99,null],
[new Money(99, "bhd"),"currency","str","BHD",null],
[new Money(99, "KRW"),"toString","str","99",null],
[new Money(99, "KRW"),"toDecimal","str","99",null],
[new Money(99, "KRW"),"amount","int",99,null],
[new Money(99, "KRW"),"currency","str","KRW",null],
[new Money(99, "GBP"),"toString","str","0.99",null],
[new Money(99, "GBP"),"toDecimal","str","0.99",null],
[new Money(99, "GBP"),"amount","int",99,null],
[new Money(99, "GBP"),"currency","str","GBP",null],
[new Money(99, "CHF"),"toString","str","0.99",null],
[new Money(99, "CHF"),"toDecimal","str","0.99",null],
[new Money(99, "CHF"),"amount","int",99,null],
[new Money(99, "CHF"),"currency","str","CHF",null],
[new Money(99, "AAA"),"toString","str","0.99",null],
[new Money(99, "AAA"),"toDecimal","str","0.99",null],
[new Money(99, "AAA"),"amount","int",99,null],
[new Money(99, "AAA"),"currency","str","AAA",null],
[new Money(99, "ZZZ"),"toString","str","0.99",null],
[new Money(99, "ZZZ"),"toDecimal","str","0.99",null],
[new Money(99, "ZZZ"),"amount","int",99,null],
[new Money(99, "ZZZ"),"currency","str","ZZZ",null],
[new Money(99, "XXX"),"toString","str","0.99",null],
[new Money(99, "XXX"),"toDecimal","str","0.99",null],
[new Money(99, "XXX"),"amount","int",99,null],
[new Money(99, "XXX"),"currency","str","XXX",null],
[new Money(100, "USD"),"toString","str","1.00",null],
[new Money(100, "USD"),"toDecimal","str","1",null],
[new Money(100, "USD"),"amount","int",100,null],
[new Money(100, "USD"),"currency","str","USD",null],
[new Money(100, "usd"),"toString","str","1.00",null],
[new Money(100, "usd"),"toDecimal","str","1",null],
[new Money(100, "usd"),"amount","int",100,null],
[new Money(100, "usd"),"currency","str","USD",null],
[new Money(100, "EUR"),"toString","str","1.00",null],
[new Money(100, "EUR"),"toDecimal","str","1",null],
[new Money(100, "EUR"),"amount","int",100,null],
[new Money(100, "EUR"),"currency","str","EUR",null],
[new Money(100, "JPY"),"toString","str","100",null],
[new Money(100, "JPY"),"toDecimal","str","100",null],
[new Money(100, "JPY"),"amount","int",100,null],
[new Money(100, "JPY"),"currency","str","JPY",null],
[new Money(100, "jpy"),"toString","str","100",null],
[new Money(100, "jpy"),"toDecimal","str","100",null],
[new Money(100, "jpy"),"amount","int",100,null],
[new Money(100, "jpy"),"currency","str","JPY",null],
[new Money(100, "BHD"),"toString","str","0.100",null],
[new Money(100, "BHD"),"toDecimal","str","0.1",null],
[new Money(100, "BHD"),"amount","int",100,null],
[new Money(100, "BHD"),"currency","str","BHD",null],
[new Money(100, "bhd"),"toString","str","0.100",null],
[new Money(100, "bhd"),"toDecimal","str","0.1",null],
[new Money(100, "bhd"),"amount","int",100,null],
[new Money(100, "bhd"),"currency","str","BHD",null],
[new Money(100, "KRW"),"toString","str","100",null],
[new Money(100, "KRW"),"toDecimal","str","100",null],
[new Money(100, "KRW"),"amount","int",100,null],
[new Money(100, "KRW"),"currency","str","KRW",null],
[new Money(100, "GBP"),"toString","str","1.00",null],
[new Money(100, "GBP"),"toDecimal","str","1",null],
[new Money(100, "GBP"),"amount","int",100,null],
[new Money(100, "GBP"),"currency","str","GBP",null],
[new Money(100, "CHF"),"toString","str","1.00",null],
[new Money(100, "CHF"),"toDecimal","str","1",null],
[new Money(100, "CHF"),"amount","int",100,null],
[new Money(100, "CHF"),"currency","str","CHF",null],
[new Money(100, "AAA"),"toString","str","1.00",null],
[new Money(100, "AAA"),"toDecimal","str","1",null],
[new Money(100, "AAA"),"amount","int",100,null],
[new Money(100, "AAA"),"currency","str","AAA",null],
[new Money(100, "ZZZ"),"toString","str","1.00",null],
[new Money(100, "ZZZ"),"toDecimal","str","1",null],
[new Money(100, "ZZZ"),"amount","int",100,null],
[new Money(100, "ZZZ"),"currency","str","ZZZ",null],
[new Money(100, "XXX"),"toString","str","1.00",null],
[new Money(100, "XXX"),"toDecimal","str","1",null],
[new Money(100, "XXX"),"amount","int",100,null],
[new Money(100, "XXX"),"currency","str","XXX",null],
[new Money(-100, "USD"),"toString","str","-1.00",null],
[new Money(-100, "USD"),"toDecimal","str","-1",null],
[new Money(-100, "USD"),"amount","int",-100,null],
[new Money(-100, "USD"),"currency","str","USD",null],
[new Money(-100, "usd"),"toString","str","-1.00",null],
[new Money(-100, "usd"),"toDecimal","str","-1",null],
[new Money(-100, "usd"),"amount","int",-100,null],
[new Money(-100, "usd"),"currency","str","USD",null],
[new Money(-100, "EUR"),"toString","str","-1.00",null],
[new Money(-100, "EUR"),"toDecimal","str","-1",null],
[new Money(-100, "EUR"),"amount","int",-100,null],
[new Money(-100, "EUR"),"currency","str","EUR",null],
[new Money(-100, "JPY"),"toString","str","-100",null],
[new Money(-100, "JPY"),"toDecimal","str","-100",null],
[new Money(-100, "JPY"),"amount","int",-100,null],
[new Money(-100, "JPY"),"currency","str","JPY",null],
[new Money(-100, "jpy"),"toString","str","-100",null],
[new Money(-100, "jpy"),"toDecimal","str","-100",null],
[new Money(-100, "jpy"),"amount","int",-100,null],
[new Money(-100, "jpy"),"currency","str","JPY",null],
[new Money(-100, "BHD"),"toString","str","-0.100",null],
[new Money(-100, "BHD"),"toDecimal","str","-0.1",null],
[new Money(-100, "BHD"),"amount","int",-100,null],
[new Money(-100, "BHD"),"currency","str","BHD",null],
[new Money(-100, "bhd"),"toString","str","-0.100",null],
[new Money(-100, "bhd"),"toDecimal","str","-0.1",null],
[new Money(-100, "bhd"),"amount","int",-100,null],
[new Money(-100, "bhd"),"currency","str","BHD",null],
[new Money(-100, "KRW"),"toString","str","-100",null],
[new Money(-100, "KRW"),"toDecimal","str","-100",null],
[new Money(-100, "KRW"),"amount","int",-100,null],
[new Money(-100, "KRW"),"currency","str","KRW",null],
[new Money(-100, "GBP"),"toString","str","-1.00",null],
[new Money(-100, "GBP"),"toDecimal","str","-1",null],
[new Money(-100, "GBP"),"amount","int",-100,null],
[new Money(-100, "GBP"),"currency","str","GBP",null],
[new Money(-100, "CHF"),"toString","str","-1.00",null],
[new Money(-100, "CHF"),"toDecimal","str","-1",null],
[new Money(-100, "CHF"),"amount","int",-100,null],
[new Money(-100, "CHF"),"currency","str","CHF",null],
[new Money(-100, "AAA"),"toString","str","-1.00",null],
[new Money(-100, "AAA"),"toDecimal","str","-1",null],
[new Money(-100, "AAA"),"amount","int",-100,null],
[new Money(-100, "AAA"),"currency","str","AAA",null],
[new Money(-100, "ZZZ"),"toString","str","-1.00",null],
[new Money(-100, "ZZZ"),"toDecimal","str","-1",null],
[new Money(-100, "ZZZ"),"amount","int",-100,null],
[new Money(-100, "ZZZ"),"currency","str","ZZZ",null],
[new Money(-100, "XXX"),"toString","str","-1.00",null],
[new Money(-100, "XXX"),"toDecimal","str","-1",null],
[new Money(-100, "XXX"),"amount","int",-100,null],
[new Money(-100, "XXX"),"currency","str","XXX",null],
[new Money(1999, "USD"),"toString","str","19.99",null],
[new Money(1999, "USD"),"toDecimal","str","19.99",null],
[new Money(1999, "USD"),"amount","int",1999,null],
[new Money(1999, "USD"),"currency","str","USD",null],
[new Money(1999, "usd"),"toString","str","19.99",null],
[new Money(1999, "usd"),"toDecimal","str","19.99",null],
[new Money(1999, "usd"),"amount","int",1999,null],
[new Money(1999, "usd"),"currency","str","USD",null],
[new Money(1999, "EUR"),"toString","str","19.99",null],
[new Money(1999, "EUR"),"toDecimal","str","19.99",null],
[new Money(1999, "EUR"),"amount","int",1999,null],
[new Money(1999, "EUR"),"currency","str","EUR",null],
[new Money(1999, "JPY"),"toString","str","1999",null],
[new Money(1999, "JPY"),"toDecimal","str","1999",null],
[new Money(1999, "JPY"),"amount","int",1999,null],
[new Money(1999, "JPY"),"currency","str","JPY",null],
[new Money(1999, "jpy"),"toString","str","1999",null],
[new Money(1999, "jpy"),"toDecimal","str","1999",null],
[new Money(1999, "jpy"),"amount","int",1999,null],
[new Money(1999, "jpy"),"currency","str","JPY",null],
[new Money(1999, "BHD"),"toString","str","1.999",null],
[new Money(1999, "BHD"),"toDecimal","str","1.999",null],
[new Money(1999, "BHD"),"amount","int",1999,null],
[new Money(1999, "BHD"),"currency","str","BHD",null],
[new Money(1999, "bhd"),"toString","str","1.999",null],
[new Money(1999, "bhd"),"toDecimal","str","1.999",null],
[new Money(1999, "bhd"),"amount","int",1999,null],
[new Money(1999, "bhd"),"currency","str","BHD",null],
[new Money(1999, "KRW"),"toString","str","1999",null],
[new Money(1999, "KRW"),"toDecimal","str","1999",null],
[new Money(1999, "KRW"),"amount","int",1999,null],
[new Money(1999, "KRW"),"currency","str","KRW",null],
[new Money(1999, "GBP"),"toString","str","19.99",null],
[new Money(1999, "GBP"),"toDecimal","str","19.99",null],
[new Money(1999, "GBP"),"amount","int",1999,null],
[new Money(1999, "GBP"),"currency","str","GBP",null],
[new Money(1999, "CHF"),"toString","str","19.99",null],
[new Money(1999, "CHF"),"toDecimal","str","19.99",null],
[new Money(1999, "CHF"),"amount","int",1999,null],
[new Money(1999, "CHF"),"currency","str","CHF",null],
[new Money(1999, "AAA"),"toString","str","19.99",null],
[new Money(1999, "AAA"),"toDecimal","str","19.99",null],
[new Money(1999, "AAA"),"amount","int",1999,null],
[new Money(1999, "AAA"),"currency","str","AAA",null],
[new Money(1999, "ZZZ"),"toString","str","19.99",null],
[new Money(1999, "ZZZ"),"toDecimal","str","19.99",null],
[new Money(1999, "ZZZ"),"amount","int",1999,null],
[new Money(1999, "ZZZ"),"currency","str","ZZZ",null],
[new Money(1999, "XXX"),"toString","str","19.99",null],
[new Money(1999, "XXX"),"toDecimal","str","19.99",null],
[new Money(1999, "XXX"),"amount","int",1999,null],
[new Money(1999, "XXX"),"currency","str","XXX",null],
[new Money(12345, "USD"),"toString","str","123.45",null],
[new Money(12345, "USD"),"toDecimal","str","123.45",null],
[new Money(12345, "USD"),"amount","int",12345,null],
[new Money(12345, "USD"),"currency","str","USD",null],
[new Money(12345, "usd"),"toString","str","123.45",null],
[new Money(12345, "usd"),"toDecimal","str","123.45",null],
[new Money(12345, "usd"),"amount","int",12345,null],
[new Money(12345, "usd"),"currency","str","USD",null],
[new Money(12345, "EUR"),"toString","str","123.45",null],
[new Money(12345, "EUR"),"toDecimal","str","123.45",null],
[new Money(12345, "EUR"),"amount","int",12345,null],
[new Money(12345, "EUR"),"currency","str","EUR",null],
[new Money(12345, "JPY"),"toString","str","12345",null],
[new Money(12345, "JPY"),"toDecimal","str","12345",null],
[new Money(12345, "JPY"),"amount","int",12345,null],
[new Money(12345, "JPY"),"currency","str","JPY",null],
[new Money(12345, "jpy"),"toString","str","12345",null],
[new Money(12345, "jpy"),"toDecimal","str","12345",null],
[new Money(12345, "jpy"),"amount","int",12345,null],
[new Money(12345, "jpy"),"currency","str","JPY",null],
[new Money(12345, "BHD"),"toString","str","12.345",null],
[new Money(12345, "BHD"),"toDecimal","str","12.345",null],
[new Money(12345, "BHD"),"amount","int",12345,null],
[new Money(12345, "BHD"),"currency","str","BHD",null],
[new Money(12345, "bhd"),"toString","str","12.345",null],
[new Money(12345, "bhd"),"toDecimal","str","12.345",null],
[new Money(12345, "bhd"),"amount","int",12345,null],
[new Money(12345, "bhd"),"currency","str","BHD",null],
[new Money(12345, "KRW"),"toString","str","12345",null],
[new Money(12345, "KRW"),"toDecimal","str","12345",null],
[new Money(12345, "KRW"),"amount","int",12345,null],
[new Money(12345, "KRW"),"currency","str","KRW",null],
[new Money(12345, "GBP"),"toString","str","123.45",null],
[new Money(12345, "GBP"),"toDecimal","str","123.45",null],
[new Money(12345, "GBP"),"amount","int",12345,null],
[new Money(12345, "GBP"),"currency","str","GBP",null],
[new Money(12345, "CHF"),"toString","str","123.45",null],
[new Money(12345, "CHF"),"toDecimal","str","123.45",null],
[new Money(12345, "CHF"),"amount","int",12345,null],
[new Money(12345, "CHF"),"currency","str","CHF",null],
[new Money(12345, "AAA"),"toString","str","123.45",null],
[new Money(12345, "AAA"),"toDecimal","str","123.45",null],
[new Money(12345, "AAA"),"amount","int",12345,null],
[new Money(12345, "AAA"),"currency","str","AAA",null],
[new Money(12345, "ZZZ"),"toString","str","123.45",null],
[new Money(12345, "ZZZ"),"toDecimal","str","123.45",null],
[new Money(12345, "ZZZ"),"amount","int",12345,null],
[new Money(12345, "ZZZ"),"currency","str","ZZZ",null],
[new Money(12345, "XXX"),"toString","str","123.45",null],
[new Money(12345, "XXX"),"toDecimal","str","123.45",null],
[new Money(12345, "XXX"),"amount","int",12345,null],
[new Money(12345, "XXX"),"currency","str","XXX",null],
[new Money(-99999, "USD"),"toString","str","-999.99",null],
[new Money(-99999, "USD"),"toDecimal","str","-999.99",null],
[new Money(-99999, "USD"),"amount","int",-99999,null],
[new Money(-99999, "USD"),"currency","str","USD",null],
[new Money(-99999, "usd"),"toString","str","-999.99",null],
[new Money(-99999, "usd"),"toDecimal","str","-999.99",null],
[new Money(-99999, "usd"),"amount","int",-99999,null],
[new Money(-99999, "usd"),"currency","str","USD",null],
[new Money(-99999, "EUR"),"toString","str","-999.99",null],
[new Money(-99999, "EUR"),"toDecimal","str","-999.99",null],
[new Money(-99999, "EUR"),"amount","int",-99999,null],
[new Money(-99999, "EUR"),"currency","str","EUR",null],
[new Money(-99999, "JPY"),"toString","str","-99999",null],
[new Money(-99999, "JPY"),"toDecimal","str","-99999",null],
[new Money(-99999, "JPY"),"amount","int",-99999,null],
[new Money(-99999, "JPY"),"currency","str","JPY",null],
[new Money(-99999, "jpy"),"toString","str","-99999",null],
[new Money(-99999, "jpy"),"toDecimal","str","-99999",null],
[new Money(-99999, "jpy"),"amount","int",-99999,null],
[new Money(-99999, "jpy"),"currency","str","JPY",null],
[new Money(-99999, "BHD"),"toString","str","-99.999",null],
[new Money(-99999, "BHD"),"toDecimal","str","-99.999",null],
[new Money(-99999, "BHD"),"amount","int",-99999,null],
[new Money(-99999, "BHD"),"currency","str","BHD",null],
[new Money(-99999, "bhd"),"toString","str","-99.999",null],
[new Money(-99999, "bhd"),"toDecimal","str","-99.999",null],
[new Money(-99999, "bhd"),"amount","int",-99999,null],
[new Money(-99999, "bhd"),"currency","str","BHD",null],
[new Money(-99999, "KRW"),"toString","str","-99999",null],
[new Money(-99999, "KRW"),"toDecimal","str","-99999",null],
[new Money(-99999, "KRW"),"amount","int",-99999,null],
[new Money(-99999, "KRW"),"currency","str","KRW",null],
[new Money(-99999, "GBP"),"toString","str","-999.99",null],
[new Money(-99999, "GBP"),"toDecimal","str","-999.99",null],
[new Money(-99999, "GBP"),"amount","int",-99999,null],
[new Money(-99999, "GBP"),"currency","str","GBP",null],
[new Money(-99999, "CHF"),"toString","str","-999.99",null],
[new Money(-99999, "CHF"),"toDecimal","str","-999.99",null],
[new Money(-99999, "CHF"),"amount","int",-99999,null],
[new Money(-99999, "CHF"),"currency","str","CHF",null],
[new Money(-99999, "AAA"),"toString","str","-999.99",null],
[new Money(-99999, "AAA"),"toDecimal","str","-999.99",null],
[new Money(-99999, "AAA"),"amount","int",-99999,null],
[new Money(-99999, "AAA"),"currency","str","AAA",null],
[new Money(-99999, "ZZZ"),"toString","str","-999.99",null],
[new Money(-99999, "ZZZ"),"toDecimal","str","-999.99",null],
[new Money(-99999, "ZZZ"),"amount","int",-99999,null],
[new Money(-99999, "ZZZ"),"currency","str","ZZZ",null],
[new Money(-99999, "XXX"),"toString","str","-999.99",null],
[new Money(-99999, "XXX"),"toDecimal","str","-999.99",null],
[new Money(-99999, "XXX"),"amount","int",-99999,null],
[new Money(-99999, "XXX"),"currency","str","XXX",null],
[new Money(123456789, "USD"),"toString","str","1234567.89",null],
[new Money(123456789, "USD"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "USD"),"amount","int",123456789,null],
[new Money(123456789, "USD"),"currency","str","USD",null],
[new Money(123456789, "usd"),"toString","str","1234567.89",null],
[new Money(123456789, "usd"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "usd"),"amount","int",123456789,null],
[new Money(123456789, "usd"),"currency","str","USD",null],
[new Money(123456789, "EUR"),"toString","str","1234567.89",null],
[new Money(123456789, "EUR"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "EUR"),"amount","int",123456789,null],
[new Money(123456789, "EUR"),"currency","str","EUR",null],
[new Money(123456789, "JPY"),"toString","str","123456789",null],
[new Money(123456789, "JPY"),"toDecimal","str","123456789",null],
[new Money(123456789, "JPY"),"amount","int",123456789,null],
[new Money(123456789, "JPY"),"currency","str","JPY",null],
[new Money(123456789, "jpy"),"toString","str","123456789",null],
[new Money(123456789, "jpy"),"toDecimal","str","123456789",null],
[new Money(123456789, "jpy"),"amount","int",123456789,null],
[new Money(123456789, "jpy"),"currency","str","JPY",null],
[new Money(123456789, "BHD"),"toString","str","123456.789",null],
[new Money(123456789, "BHD"),"toDecimal","str","123456.789",null],
[new Money(123456789, "BHD"),"amount","int",123456789,null],
[new Money(123456789, "BHD"),"currency","str","BHD",null],
[new Money(123456789, "bhd"),"toString","str","123456.789",null],
[new Money(123456789, "bhd"),"toDecimal","str","123456.789",null],
[new Money(123456789, "bhd"),"amount","int",123456789,null],
[new Money(123456789, "bhd"),"currency","str","BHD",null],
[new Money(123456789, "KRW"),"toString","str","123456789",null],
[new Money(123456789, "KRW"),"toDecimal","str","123456789",null],
[new Money(123456789, "KRW"),"amount","int",123456789,null],
[new Money(123456789, "KRW"),"currency","str","KRW",null],
[new Money(123456789, "GBP"),"toString","str","1234567.89",null],
[new Money(123456789, "GBP"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "GBP"),"amount","int",123456789,null],
[new Money(123456789, "GBP"),"currency","str","GBP",null],
[new Money(123456789, "CHF"),"toString","str","1234567.89",null],
[new Money(123456789, "CHF"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "CHF"),"amount","int",123456789,null],
[new Money(123456789, "CHF"),"currency","str","CHF",null],
[new Money(123456789, "AAA"),"toString","str","1234567.89",null],
[new Money(123456789, "AAA"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "AAA"),"amount","int",123456789,null],
[new Money(123456789, "AAA"),"currency","str","AAA",null],
[new Money(123456789, "ZZZ"),"toString","str","1234567.89",null],
[new Money(123456789, "ZZZ"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "ZZZ"),"amount","int",123456789,null],
[new Money(123456789, "ZZZ"),"currency","str","ZZZ",null],
[new Money(123456789, "XXX"),"toString","str","1234567.89",null],
[new Money(123456789, "XXX"),"toDecimal","str","1234567.89",null],
[new Money(123456789, "XXX"),"amount","int",123456789,null],
[new Money(123456789, "XXX"),"currency","str","XXX",null],
[new Money(-123456789, "USD"),"toString","str","-1234567.89",null],
[new Money(-123456789, "USD"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "USD"),"amount","int",-123456789,null],
[new Money(-123456789, "USD"),"currency","str","USD",null],
[new Money(-123456789, "usd"),"toString","str","-1234567.89",null],
[new Money(-123456789, "usd"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "usd"),"amount","int",-123456789,null],
[new Money(-123456789, "usd"),"currency","str","USD",null],
[new Money(-123456789, "EUR"),"toString","str","-1234567.89",null],
[new Money(-123456789, "EUR"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "EUR"),"amount","int",-123456789,null],
[new Money(-123456789, "EUR"),"currency","str","EUR",null],
[new Money(-123456789, "JPY"),"toString","str","-123456789",null],
[new Money(-123456789, "JPY"),"toDecimal","str","-123456789",null],
[new Money(-123456789, "JPY"),"amount","int",-123456789,null],
[new Money(-123456789, "JPY"),"currency","str","JPY",null],
[new Money(-123456789, "jpy"),"toString","str","-123456789",null],
[new Money(-123456789, "jpy"),"toDecimal","str","-123456789",null],
[new Money(-123456789, "jpy"),"amount","int",-123456789,null],
[new Money(-123456789, "jpy"),"currency","str","JPY",null],
[new Money(-123456789, "BHD"),"toString","str","-123456.789",null],
[new Money(-123456789, "BHD"),"toDecimal","str","-123456.789",null],
[new Money(-123456789, "BHD"),"amount","int",-123456789,null],
[new Money(-123456789, "BHD"),"currency","str","BHD",null],
[new Money(-123456789, "bhd"),"toString","str","-123456.789",null],
[new Money(-123456789, "bhd"),"toDecimal","str","-123456.789",null],
[new Money(-123456789, "bhd"),"amount","int",-123456789,null],
[new Money(-123456789, "bhd"),"currency","str","BHD",null],
[new Money(-123456789, "KRW"),"toString","str","-123456789",null],
[new Money(-123456789, "KRW"),"toDecimal","str","-123456789",null],
[new Money(-123456789, "KRW"),"amount","int",-123456789,null],
[new Money(-123456789, "KRW"),"currency","str","KRW",null],
[new Money(-123456789, "GBP"),"toString","str","-1234567.89",null],
[new Money(-123456789, "GBP"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "GBP"),"amount","int",-123456789,null],
[new Money(-123456789, "GBP"),"currency","str","GBP",null],
[new Money(-123456789, "CHF"),"toString","str","-1234567.89",null],
[new Money(-123456789, "CHF"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "CHF"),"amount","int",-123456789,null],
[new Money(-123456789, "CHF"),"currency","str","CHF",null],
[new Money(-123456789, "AAA"),"toString","str","-1234567.89",null],
[new Money(-123456789, "AAA"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "AAA"),"amount","int",-123456789,null],
[new Money(-123456789, "AAA"),"currency","str","AAA",null],
[new Money(-123456789, "ZZZ"),"toString","str","-1234567.89",null],
[new Money(-123456789, "ZZZ"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "ZZZ"),"amount","int",-123456789,null],
[new Money(-123456789, "ZZZ"),"currency","str","ZZZ",null],
[new Money(-123456789, "XXX"),"toString","str","-1234567.89",null],
[new Money(-123456789, "XXX"),"toDecimal","str","-1234567.89",null],
[new Money(-123456789, "XXX"),"amount","int",-123456789,null],
[new Money(-123456789, "XXX"),"currency","str","XXX",null]
];

for (var i = 0; i < Q.length; i++) {
  var mm = eval(Q[i][0]), fn = Q[i][1], kind = Q[i][2], want = Q[i][3];
  var msg = Q[i][0] + "." + fn;
  var got;
  try { got = mm[fn](); if (fn === "toDecimal") got = got.toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (kind === "int") assert_eq(got, want, msg);
  else assert_eq(got, want, msg);
}

var FM = [
[new Money(1999, 'USD'),"$19.99"],
[new Money(-1999, 'USD'),"-$19.99"],
[new Money(5, 'USD'),"$0.05"],
[new Money(-5, 'USD'),"-$0.05"],
[new Money(0, 'USD'),"$0.00"],
[new Money(1999, 'EUR'),"\u20ac19.99"],
[new Money(-1999, 'EUR'),"-\u20ac19.99"],
[new Money(5, 'EUR'),"\u20ac0.05"],
[new Money(-5, 'EUR'),"-\u20ac0.05"],
[new Money(0, 'EUR'),"\u20ac0.00"],
[new Money(1999, 'GBP'),"\u00a319.99"],
[new Money(-1999, 'GBP'),"-\u00a319.99"],
[new Money(5, 'GBP'),"\u00a30.05"],
[new Money(-5, 'GBP'),"-\u00a30.05"],
[new Money(0, 'GBP'),"\u00a30.00"],
[new Money(1999, 'JPY'),"\u00a51999"],
[new Money(-1999, 'JPY'),"-\u00a51999"],
[new Money(5, 'JPY'),"\u00a55"],
[new Money(-5, 'JPY'),"-\u00a55"],
[new Money(0, 'JPY'),"\u00a50"],
[new Money(1999, 'CNY'),"\u00a519.99"],
[new Money(-1999, 'CNY'),"-\u00a519.99"],
[new Money(5, 'CNY'),"\u00a50.05"],
[new Money(-5, 'CNY'),"-\u00a50.05"],
[new Money(0, 'CNY'),"\u00a50.00"],
[new Money(1999, 'INR'),"\u20b919.99"],
[new Money(-1999, 'INR'),"-\u20b919.99"],
[new Money(5, 'INR'),"\u20b90.05"],
[new Money(-5, 'INR'),"-\u20b90.05"],
[new Money(0, 'INR'),"\u20b90.00"],
[new Money(1999, 'KRW'),"\u20a91999"],
[new Money(-1999, 'KRW'),"-\u20a91999"],
[new Money(5, 'KRW'),"\u20a95"],
[new Money(-5, 'KRW'),"-\u20a95"],
[new Money(0, 'KRW'),"\u20a90"],
[new Money(1999, 'CAD'),"CA$19.99"],
[new Money(-1999, 'CAD'),"-CA$19.99"],
[new Money(5, 'CAD'),"CA$0.05"],
[new Money(-5, 'CAD'),"-CA$0.05"],
[new Money(0, 'CAD'),"CA$0.00"],
[new Money(1999, 'AUD'),"A$19.99"],
[new Money(-1999, 'AUD'),"-A$19.99"],
[new Money(5, 'AUD'),"A$0.05"],
[new Money(-5, 'AUD'),"-A$0.05"],
[new Money(0, 'AUD'),"A$0.00"],
[new Money(1999, 'CHF'),"19.99 CHF"],
[new Money(5, 'CHF'),"0.05 CHF"]
];

for (var i = 0; i < FM.length; i++) {
  assert_eq(eval(FM[i][0]).format(), FM[i][1], "format#" + i);
}

var PAIR = [
["add",[1999,1],"str","20.00"],
["sub",[1999,1],"str","19.98"],
["cmp",[1999,1],"int",1],
["equals",[1999,1],"bool",false],
["add",[1999,-1],"str","19.98"],
["sub",[1999,-1],"str","20.00"],
["cmp",[1999,-1],"int",1],
["equals",[1999,-1],"bool",false],
["add",[1999,250],"str","22.49"],
["sub",[1999,250],"str","17.49"],
["cmp",[1999,250],"int",1],
["equals",[1999,250],"bool",false],
["add",[1999,-250],"str","17.49"],
["sub",[1999,-250],"str","22.49"],
["cmp",[1999,-250],"int",1],
["equals",[1999,-250],"bool",false],
["add",[1999,9007199254740992],"str","90071992547429.91"],
["sub",[1999,9007199254740992],"str","-90071992547389.93"],
["cmp",[1999,9007199254740992],"int",-1],
["equals",[1999,9007199254740992],"bool",false],
["add",[1999,-9007199254740992],"str","-90071992547389.93"],
["sub",[1999,-9007199254740992],"str","90071992547429.91"],
["cmp",[1999,-9007199254740992],"int",1],
["equals",[1999,-9007199254740992],"bool",false],
["add",[-1999,1],"str","-19.98"],
["sub",[-1999,1],"str","-20.00"],
["cmp",[-1999,1],"int",-1],
["equals",[-1999,1],"bool",false],
["add",[-1999,-1],"str","-20.00"],
["sub",[-1999,-1],"str","-19.98"],
["cmp",[-1999,-1],"int",-1],
["equals",[-1999,-1],"bool",false],
["add",[-1999,250],"str","-17.49"],
["sub",[-1999,250],"str","-22.49"],
["cmp",[-1999,250],"int",-1],
["equals",[-1999,250],"bool",false],
["add",[-1999,-250],"str","-22.49"],
["sub",[-1999,-250],"str","-17.49"],
["cmp",[-1999,-250],"int",-1],
["equals",[-1999,-250],"bool",false],
["add",[-1999,9007199254740992],"str","90071992547389.93"],
["sub",[-1999,9007199254740992],"str","-90071992547429.91"],
["cmp",[-1999,9007199254740992],"int",-1],
["equals",[-1999,9007199254740992],"bool",false],
["add",[-1999,-9007199254740992],"str","-90071992547429.91"],
["sub",[-1999,-9007199254740992],"str","90071992547389.93"],
["cmp",[-1999,-9007199254740992],"int",1],
["equals",[-1999,-9007199254740992],"bool",false],
["add",[0,1],"str","0.01"],
["sub",[0,1],"str","-0.01"],
["cmp",[0,1],"int",-1],
["equals",[0,1],"bool",false],
["add",[0,-1],"str","-0.01"],
["sub",[0,-1],"str","0.01"],
["cmp",[0,-1],"int",1],
["equals",[0,-1],"bool",false],
["add",[0,250],"str","2.50"],
["sub",[0,250],"str","-2.50"],
["cmp",[0,250],"int",-1],
["equals",[0,250],"bool",false],
["add",[0,-250],"str","-2.50"],
["sub",[0,-250],"str","2.50"],
["cmp",[0,-250],"int",1],
["equals",[0,-250],"bool",false],
["add",[0,9007199254740992],"str","90071992547409.92"],
["sub",[0,9007199254740992],"str","-90071992547409.92"],
["cmp",[0,9007199254740992],"int",-1],
["equals",[0,9007199254740992],"bool",false],
["add",[0,-9007199254740992],"str","-90071992547409.92"],
["sub",[0,-9007199254740992],"str","90071992547409.92"],
["cmp",[0,-9007199254740992],"int",1],
["equals",[0,-9007199254740992],"bool",false],
["add",[1,1],"str","0.02"],
["sub",[1,1],"str","0.00"],
["cmp",[1,1],"int",0],
["equals",[1,1],"bool",true],
["add",[1,-1],"str","0.00"],
["sub",[1,-1],"str","0.02"],
["cmp",[1,-1],"int",1],
["equals",[1,-1],"bool",false],
["add",[1,250],"str","2.51"],
["sub",[1,250],"str","-2.49"],
["cmp",[1,250],"int",-1],
["equals",[1,250],"bool",false],
["add",[1,-250],"str","-2.49"],
["sub",[1,-250],"str","2.51"],
["cmp",[1,-250],"int",1],
["equals",[1,-250],"bool",false],
["add",[1,9007199254740992],"str","90071992547409.93"],
["sub",[1,9007199254740992],"str","-90071992547409.91"],
["cmp",[1,9007199254740992],"int",-1],
["equals",[1,9007199254740992],"bool",false],
["add",[1,-9007199254740992],"str","-90071992547409.91"],
["sub",[1,-9007199254740992],"str","90071992547409.93"],
["cmp",[1,-9007199254740992],"int",1],
["equals",[1,-9007199254740992],"bool",false],
["add",[1000000,1],"str","10000.01"],
["sub",[1000000,1],"str","9999.99"],
["cmp",[1000000,1],"int",1],
["equals",[1000000,1],"bool",false],
["add",[1000000,-1],"str","9999.99"],
["sub",[1000000,-1],"str","10000.01"],
["cmp",[1000000,-1],"int",1],
["equals",[1000000,-1],"bool",false],
["add",[1000000,250],"str","10002.50"],
["sub",[1000000,250],"str","9997.50"],
["cmp",[1000000,250],"int",1],
["equals",[1000000,250],"bool",false],
["add",[1000000,-250],"str","9997.50"],
["sub",[1000000,-250],"str","10002.50"],
["cmp",[1000000,-250],"int",1],
["equals",[1000000,-250],"bool",false],
["add",[1000000,9007199254740992],"str","90071992557409.92"],
["sub",[1000000,9007199254740992],"str","-90071992537409.92"],
["cmp",[1000000,9007199254740992],"int",-1],
["equals",[1000000,9007199254740992],"bool",false],
["add",[1000000,-9007199254740992],"str","-90071992537409.92"],
["sub",[1000000,-9007199254740992],"str","90071992557409.92"],
["cmp",[1000000,-9007199254740992],"int",1],
["equals",[1000000,-9007199254740992],"bool",false]
];

for (var i = 0; i < PAIR.length; i++) {
  var fn = PAIR[i][0], amt = PAIR[i][1], kind = PAIR[i][2], want = PAIR[i][3];
  var a = new Money(amt[0], "USD"), b = new Money(amt[1], "USD");
  var msg = fn + "(" + amt[0] + "," + amt[1] + ")";
  if (kind === "throw") {
    if (fn === "cmp" || fn === "equals") { __fail++; __out("FAIL bad pair kind " + msg); continue; }
    assert_throws(function () { a[fn](b); }, want, msg);
    continue;
  }
  var got;
  try { got = a[fn](b); if (fn !== "cmp" && fn !== "equals") got = got.toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (kind === "bool") assert_true(got === want, msg);
  else if (kind === "int") assert_eq(got, want, msg);
  else assert_eq(got, want, msg);
}

var MUL = [
[1999,"0","str","0.00"],
[1999,"1","str","19.99"],
[1999,"-1","str","-19.99"],
[1999,"2","str","39.98"],
[1999,"10","str","199.90"],
[1999,"-7","str","-139.93"],
[-1999,"0","str","0.00"],
[-1999,"1","str","-19.99"],
[-1999,"-1","str","19.99"],
[-1999,"2","str","-39.98"],
[-1999,"10","str","-199.90"],
[-1999,"-7","str","139.93"],
[0,"0","str","0.00"],
[0,"1","str","0.00"],
[0,"-1","str","0.00"],
[0,"2","str","0.00"],
[0,"10","str","0.00"],
[0,"-7","str","0.00"],
[3,"0","str","0.00"],
[3,"1","str","0.03"],
[3,"-1","str","-0.03"],
[3,"2","str","0.06"],
[3,"10","str","0.30"],
[3,"-7","str","-0.21"],
[100,"1.5","throw","RangeError"],
[100,"0.0001","throw","RangeError"],
[100,"nan","throw","RangeError"],
[100,"inf","throw","RangeError"]
];

for (var i = 0; i < MUL.length; i++) {
  var a = MUL[i][0], k = parseFloat(MUL[i][1]), kind = MUL[i][2], want = MUL[i][3];
  var msg = "mul(" + a + "," + k + ")";
  if (kind === "throw") { assert_throws(function () { new Money(a, "USD").mul(k); }, want, msg); continue; }
  var got;
  try { got = new Money(a, "USD").mul(k).toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  assert_eq(got, want, msg);
}

var AL = [
[1000,[1,1,1],1000],
[1000,[1,2],1000],
[1000,[0,1,1],1000],
[1000,[3,3,3],1000],
[1000,[1,0,0,2],1000],
[1000,[5],1000],
[1000,[1,1,1,1,1,1,1],1000],
[1000,[2,3],1000],
[1000,[7,0,3],1000],
[1999,[1,1,1],1999],
[1999,[1,2],1999],
[1999,[0,1,1],1999],
[1999,[3,3,3],1999],
[1999,[1,0,0,2],1999],
[1999,[5],1999],
[1999,[1,1,1,1,1,1,1],1999],
[1999,[2,3],1999],
[1999,[7,0,3],1999],
[-1000,[1,1,1],-1000],
[-1000,[1,2],-1000],
[-1000,[0,1,1],-1000],
[-1000,[3,3,3],-1000],
[-1000,[1,0,0,2],-1000],
[-1000,[5],-1000],
[-1000,[1,1,1,1,1,1,1],-1000],
[-1000,[2,3],-1000],
[-1000,[7,0,3],-1000],
[7,[1,1,1],7],
[7,[1,2],7],
[7,[0,1,1],7],
[7,[3,3,3],7],
[7,[1,0,0,2],7],
[7,[5],7],
[7,[1,1,1,1,1,1,1],7],
[7,[2,3],7],
[7,[7,0,3],7],
[1,[1,1,1],1],
[1,[1,2],1],
[1,[0,1,1],1],
[1,[3,3,3],1],
[1,[1,0,0,2],1],
[1,[5],1],
[1,[1,1,1,1,1,1,1],1],
[1,[2,3],1],
[1,[7,0,3],1],
[0,[1,1,1],0],
[0,[1,2],0],
[0,[0,1,1],0],
[0,[3,3,3],0],
[0,[1,0,0,2],0],
[0,[5],0],
[0,[1,1,1,1,1,1,1],0],
[0,[2,3],0],
[0,[7,0,3],0],
[100000,[1,1,1],100000],
[100000,[1,2],100000],
[100000,[0,1,1],100000],
[100000,[3,3,3],100000],
[100000,[1,0,0,2],100000],
[100000,[5],100000],
[100000,[1,1,1,1,1,1,1],100000],
[100000,[2,3],100000],
[100000,[7,0,3],100000],
[-3,[1,1,1],-3],
[-3,[1,2],-3],
[-3,[0,1,1],-3],
[-3,[3,3,3],-3],
[-3,[1,0,0,2],-3],
[-3,[5],-3],
[-3,[1,1,1,1,1,1,1],-3],
[-3,[2,3],-3],
[-3,[7,0,3],-3]
];

for (var i = 0; i < AL.length; i++) {
  var a = AL[i][0], shares = AL[i][1], total = AL[i][2];
  var msg = "allocate(" + a + ",[" + shares + "])";
  var parts;
  try { parts = new Money(a, "USD").allocate(shares).map(function (m) { return m.amount(); }); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  var sum = 0;
  for (var j = 0; j < parts.length; j++) sum += parts[j];
  assert_eq(sum, total, msg + " exact-sum invariant");
  var base = Math.abs(total) / shares.length | 0;
  assert_true(parts.every(function (p) { return p <= Math.abs(a) && p >= -Math.abs(a); }),
              msg + " no share exceeds the amount magnitude");
}

var ALB = [
["[]","RangeError"],
["[0, 0]","RangeError"],
["[1.5, 1]","RangeError"],
["[-1, 2]","RangeError"],
["[1, -0.5]","RangeError"],
["[0]","RangeError"]
];

for (var i = 0; i < ALB.length; i++) {
  assert_throws(function () { new Money(100, "USD").allocate(eval(ALB[i][0])); },
                ALB[i][1], "allocate rejects " + ALB[i][0]);
}


// cross-currency is a missing exchange rate, not arithmetic
assert_throws(function () { new Money(1, "USD").add(new Money(1, "EUR")); }, "TypeError", "cross-currency add");
assert_throws(function () { new Money(1, "USD").sub(new Money(1, "EUR")); }, "TypeError", "cross-currency sub");
// same code, different minorDigits scale
assert_throws(function () {
  new Money(1, "USD", { minorDigits: 2 }).add(new Money(1, "USD", { minorDigits: 6 }));
}, "TypeError", "scale mismatch");
// currency validation
assert_throws(function () { new Money(1, "US"); }, "RangeError", "currency 2 letters");
assert_throws(function () { new Money(1, "USDD"); }, "RangeError", "currency 4 letters");
assert_throws(function () { new Money(1, "U$D"); }, "RangeError", "currency symbol");
assert_throws(function () { new Money(1, 42); }, "TypeError", "currency non-string");
assert_eq(new Money(1, "usd").currency(), "USD", "currency lowercased input uppercased");
// minorDigits bounds
assert_throws(function () { new Money(1, "USD", { minorDigits: 7 }); }, "RangeError", "minorDigits 7");
assert_throws(function () { new Money(1, "USD", { minorDigits: -1 }); }, "RangeError", "minorDigits -1");
assert_eq(new Money(0, "USD", { minorDigits: 0 }).toString(), "0", "minorDigits 0");
assert_eq(new Money(1, "USD", { minorDigits: 0 }).toString(), "1", "minorDigits 0 amount 1");
assert_eq(new Money(1, "USD", { minorDigits: 6 }).toString(), "0.000001", "minorDigits 6");
// minorUnits must be an integer count
assert_throws(function () { new Money(1.5, "USD"); }, "RangeError", "fractional cent");
assert_throws(function () { new Money(NaN, "USD"); }, "RangeError", "NaN cent");
assert_throws(function () { new Money("100", "USD"); }, "TypeError", "string minorUnits");
// operand must be a Money
assert_throws(function () { new Money(1, "USD").add(1); }, "TypeError", "add non-Money");
assert_throws(function () { new Money(1, "USD").cmp(new Decimal(1)); }, "TypeError", "cmp Decimal");
// toJSON is toString
assert_eq(JSON.stringify({ m: new Money(1999, "USD") }), '{"m":"19.99"}', "toJSON");
// toDecimal is exact at every minorDigits
for (var md = 0; md <= 6; md++) {
  var d = new Money(123, "USD", { minorDigits: md }).toDecimal().toString();
  var div = [1, 10, 100, 1000, 10000, 100000, 1000000][md];
  var w = Math.floor(123 / div), f = 123 % div;
  var want = String(w) + (md ? "." + String(f).padStart(md, "0") : "");
  assert_eq(d, want, "toDecimal minorDigits=" + md);
}
// INT64_MIN mul(-1) must overflow, not wrap
assert_throws(function () { new Money(-9223372036854775808, "USD").mul(-1); },
              "RangeError", "mul(-1) of INT64_MIN");
// INT64 edge: add to INT64_MAX
assert_throws(function () {
  new Money(9223372036854775807, "USD").add(new Money(1, "USD"));
}, "RangeError", "add overflow at INT64_MAX");

summary(__TAG); // 965 cases
