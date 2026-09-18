// GENERATED probe (df_groupby) -- do not edit; run materialize.sh
import { DataFrame } from "dyna:dataframe";
var __TAG = "df_groupby";
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
var g0 = new DataFrame({ cat: ["b"], x: new Float64Array([12.0]) });
var mk0 = new Uint8Array([1]);
var gb0 = g0.GROUP_BY_SUM("cat", "x", mk0); assert_arr_eq(gb0.keys, ["b"], "gb keys t0"); assert_arr_close(Array.from(gb0.values), [12.0], 1e-9, 1e-12, "gb sum t0");
assert_arr_close(Array.from(g0.GROUP_BY_MEAN("cat", "x", mk0).values), [12.0], 1e-9, 1e-12, "gb mean t0");
assert_arr_close(Array.from(g0.GROUP_BY_MIN("cat", "x", mk0).values), [12.0], 1e-9, 0, "gb min t0");
assert_arr_close(Array.from(g0.GROUP_BY_MAX("cat", "x", mk0).values), [12.0], 1e-9, 0, "gb max t0");
assert_arr_eq(Array.from(g0.GROUP_BY_COUNT("cat", mk0).values), [1], "gb count t0");
var g1 = new DataFrame({ cat: ["a", "b"], x: new Float64Array([73.0,8.0]) });
var mk1 = new Uint8Array([1,1]);
var gb1 = g1.GROUP_BY_SUM("cat", "x", mk1); assert_arr_eq(gb1.keys, ["a", "b"], "gb keys t1"); assert_arr_close(Array.from(gb1.values), [73.0,8.0], 1e-9, 1e-12, "gb sum t1");
assert_arr_close(Array.from(g1.GROUP_BY_MEAN("cat", "x", mk1).values), [73.0,8.0], 1e-9, 1e-12, "gb mean t1");
assert_arr_close(Array.from(g1.GROUP_BY_MIN("cat", "x", mk1).values), [73.0,8.0], 1e-9, 0, "gb min t1");
assert_arr_close(Array.from(g1.GROUP_BY_MAX("cat", "x", mk1).values), [73.0,8.0], 1e-9, 0, "gb max t1");
assert_arr_eq(Array.from(g1.GROUP_BY_COUNT("cat", mk1).values), [1,1], "gb count t1");
var g2 = new DataFrame({ cat: ["b", "a", "b", "a", "b"], x: new Float64Array([50.0,5.0,12.0,99.0,30.0]) });
var mk2 = new Uint8Array([0,1,1,1,1]);
var gb2 = g2.GROUP_BY_SUM("cat", "x", mk2); assert_eq(gb2.keys.length, 2, "gb key count t2");
var gi2_0 = gb2.keys.indexOf("a"); assert_true(gi2_0 >= 0, "gb key a present t2");
var gsum2_0 = Array.from(g2.GROUP_BY_SUM("cat", "x", mk2).values); assert_true(gsum2_0[gi2_0] === 104.0, "gb sum of a t2");
var gmean2_0 = Array.from(g2.GROUP_BY_MEAN("cat", "x", mk2).values); assert_true(gmean2_0[gi2_0] === 52.0, "gb mean of a t2");
var gmin2_0 = Array.from(g2.GROUP_BY_MIN("cat", "x", mk2).values); assert_true(gmin2_0[gi2_0] === 5.0, "gb min of a t2");
var gmax2_0 = Array.from(g2.GROUP_BY_MAX("cat", "x", mk2).values); assert_true(gmax2_0[gi2_0] === 99.0, "gb max of a t2");
var gc2_0 = Array.from(g2.GROUP_BY_COUNT("cat", mk2).values); assert_true(gc2_0[gi2_0] === 2, "gb count of a t2");
var gi2_1 = gb2.keys.indexOf("b"); assert_true(gi2_1 >= 0, "gb key b present t2");
var gsum2_1 = Array.from(g2.GROUP_BY_SUM("cat", "x", mk2).values); assert_true(gsum2_1[gi2_1] === 42.0, "gb sum of b t2");
var gmean2_1 = Array.from(g2.GROUP_BY_MEAN("cat", "x", mk2).values); assert_true(gmean2_1[gi2_1] === 21.0, "gb mean of b t2");
var gmin2_1 = Array.from(g2.GROUP_BY_MIN("cat", "x", mk2).values); assert_true(gmin2_1[gi2_1] === 12.0, "gb min of b t2");
var gmax2_1 = Array.from(g2.GROUP_BY_MAX("cat", "x", mk2).values); assert_true(gmax2_1[gi2_1] === 30.0, "gb max of b t2");
var gc2_1 = Array.from(g2.GROUP_BY_COUNT("cat", mk2).values); assert_true(gc2_1[gi2_1] === 2, "gb count of b t2");
var g3 = new DataFrame({ cat: ["b", "b", "b", "a", "b", "a", "c", "a", "a", "c", "b", "c"], x: new Float64Array([13.0,48.0,99.0,66.0,73.0,68.0,91.0,26.0,65.0,12.0,55.0,98.0]) });
var mk3 = new Uint8Array([0,1,1,1,1,1,1,1,1,1,1,1]);
var gb3 = g3.GROUP_BY_SUM("cat", "x", mk3); assert_eq(gb3.keys.length, 3, "gb key count t3");
var gi3_0 = gb3.keys.indexOf("b"); assert_true(gi3_0 >= 0, "gb key b present t3");
var gsum3_0 = Array.from(g3.GROUP_BY_SUM("cat", "x", mk3).values); assert_true(gsum3_0[gi3_0] === 275.0, "gb sum of b t3");
var gmean3_0 = Array.from(g3.GROUP_BY_MEAN("cat", "x", mk3).values); assert_true(gmean3_0[gi3_0] === 68.75, "gb mean of b t3");
var gmin3_0 = Array.from(g3.GROUP_BY_MIN("cat", "x", mk3).values); assert_true(gmin3_0[gi3_0] === 48.0, "gb min of b t3");
var gmax3_0 = Array.from(g3.GROUP_BY_MAX("cat", "x", mk3).values); assert_true(gmax3_0[gi3_0] === 99.0, "gb max of b t3");
var gc3_0 = Array.from(g3.GROUP_BY_COUNT("cat", mk3).values); assert_true(gc3_0[gi3_0] === 4, "gb count of b t3");
var gi3_1 = gb3.keys.indexOf("a"); assert_true(gi3_1 >= 0, "gb key a present t3");
var gsum3_1 = Array.from(g3.GROUP_BY_SUM("cat", "x", mk3).values); assert_true(gsum3_1[gi3_1] === 225.0, "gb sum of a t3");
var gmean3_1 = Array.from(g3.GROUP_BY_MEAN("cat", "x", mk3).values); assert_true(gmean3_1[gi3_1] === 56.25, "gb mean of a t3");
var gmin3_1 = Array.from(g3.GROUP_BY_MIN("cat", "x", mk3).values); assert_true(gmin3_1[gi3_1] === 26.0, "gb min of a t3");
var gmax3_1 = Array.from(g3.GROUP_BY_MAX("cat", "x", mk3).values); assert_true(gmax3_1[gi3_1] === 68.0, "gb max of a t3");
var gc3_1 = Array.from(g3.GROUP_BY_COUNT("cat", mk3).values); assert_true(gc3_1[gi3_1] === 4, "gb count of a t3");
var gi3_2 = gb3.keys.indexOf("c"); assert_true(gi3_2 >= 0, "gb key c present t3");
var gsum3_2 = Array.from(g3.GROUP_BY_SUM("cat", "x", mk3).values); assert_true(gsum3_2[gi3_2] === 201.0, "gb sum of c t3");
var gmean3_2 = Array.from(g3.GROUP_BY_MEAN("cat", "x", mk3).values); assert_true(gmean3_2[gi3_2] === 67.0, "gb mean of c t3");
var gmin3_2 = Array.from(g3.GROUP_BY_MIN("cat", "x", mk3).values); assert_true(gmin3_2[gi3_2] === 12.0, "gb min of c t3");
var gmax3_2 = Array.from(g3.GROUP_BY_MAX("cat", "x", mk3).values); assert_true(gmax3_2[gi3_2] === 98.0, "gb max of c t3");
var gc3_2 = Array.from(g3.GROUP_BY_COUNT("cat", mk3).values); assert_true(gc3_2[gi3_2] === 3, "gb count of c t3");
var g4 = new DataFrame({ cat: ["b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a", "b", "a"], x: new Float64Array([17.0,84.0,83.0,10.0,5.0,56.0,31.0,38.0,53.0,24.0,39.0,6.0,97.0,76.0,99.0,2.0,21.0,28.0,99.0,18.0,45.0,96.0,47.0,78.0,81.0,92.0,39.0,2.0,5.0,92.0,39.0,78.0,41.0,72.0,67.0,78.0,1.0,0.0,67.0,50.0]) });
var mk4 = new Uint8Array([0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]);
var gb4 = g4.GROUP_BY_SUM("cat", "x", mk4); assert_eq(gb4.keys.length, 2, "gb key count t4");
var gi4_0 = gb4.keys.indexOf("a"); assert_true(gi4_0 >= 0, "gb key a present t4");
var gsum4_0 = Array.from(g4.GROUP_BY_SUM("cat", "x", mk4).values); assert_true(gsum4_0[gi4_0] === 980.0, "gb sum of a t4");
var gmean4_0 = Array.from(g4.GROUP_BY_MEAN("cat", "x", mk4).values); assert_true(gmean4_0[gi4_0] === 49.0, "gb mean of a t4");
var gmin4_0 = Array.from(g4.GROUP_BY_MIN("cat", "x", mk4).values); assert_true(gmin4_0[gi4_0] === 0.0, "gb min of a t4");
var gmax4_0 = Array.from(g4.GROUP_BY_MAX("cat", "x", mk4).values); assert_true(gmax4_0[gi4_0] === 96.0, "gb max of a t4");
var gc4_0 = Array.from(g4.GROUP_BY_COUNT("cat", mk4).values); assert_true(gc4_0[gi4_0] === 20, "gb count of a t4");
var gi4_1 = gb4.keys.indexOf("b"); assert_true(gi4_1 >= 0, "gb key b present t4");
var gsum4_1 = Array.from(g4.GROUP_BY_SUM("cat", "x", mk4).values); assert_true(gsum4_1[gi4_1] === 959.0, "gb sum of b t4");
var gmean4_1 = Array.from(g4.GROUP_BY_MEAN("cat", "x", mk4).values); assert_true(gmean4_1[gi4_1] === 50.473684210526315, "gb mean of b t4");
var gmin4_1 = Array.from(g4.GROUP_BY_MIN("cat", "x", mk4).values); assert_true(gmin4_1[gi4_1] === 1.0, "gb min of b t4");
var gmax4_1 = Array.from(g4.GROUP_BY_MAX("cat", "x", mk4).values); assert_true(gmax4_1[gi4_1] === 99.0, "gb max of b t4");
var gc4_1 = Array.from(g4.GROUP_BY_COUNT("cat", mk4).values); assert_true(gc4_1[gi4_1] === 19, "gb count of b t4");
var g5 = new DataFrame({ cat: ["b", "c", "c", "c", "b", "a", "a", "a", "a", "c", "a", "b", "a", "a", "b", "a", "b", "b", "b", "a", "c", "c", "c", "c", "b", "a", "a", "b", "c", "c", "c", "a", "b", "a", "a", "b", "a", "b", "b", "a", "b", "c", "b", "b", "a", "a", "b", "c", "c", "b", "c", "a", "a", "a", "c", "c", "a", "c", "a", "b", "c", "c", "c", "a"], x: new Float64Array([81.0,92.0,67.0,54.0,69.0,60.0,75.0,26.0,13.0,40.0,79.0,54.0,21.0,28.0,83.0,50.0,41.0,24.0,79.0,98.0,61.0,88.0,35.0,10.0,49.0,4.0,51.0,42.0,89.0,48.0,75.0,2.0,73.0,4.0,99.0,22.0,17.0,76.0,99.0,94.0,85.0,24.0,63.0,58.0,17.0,8.0,67.0,86.0,61.0,72.0,59.0,2.0,73.0,20.0,75.0,62.0,85.0,16.0,35.0,86.0,53.0,88.0,63.0,34.0]) });
var mk5 = new Uint8Array([0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]);
var gb5 = g5.GROUP_BY_SUM("cat", "x", mk5); assert_eq(gb5.keys.length, 3, "gb key count t5");
var gi5_0 = gb5.keys.indexOf("c"); assert_true(gi5_0 >= 0, "gb key c present t5");
var gsum5_0 = Array.from(g5.GROUP_BY_SUM("cat", "x", mk5).values); assert_true(gsum5_0[gi5_0] === 1246.0, "gb sum of c t5");
var gmean5_0 = Array.from(g5.GROUP_BY_MEAN("cat", "x", mk5).values); assert_true(gmean5_0[gi5_0] === 59.333333333333336, "gb mean of c t5");
var gmin5_0 = Array.from(g5.GROUP_BY_MIN("cat", "x", mk5).values); assert_true(gmin5_0[gi5_0] === 10.0, "gb min of c t5");
var gmax5_0 = Array.from(g5.GROUP_BY_MAX("cat", "x", mk5).values); assert_true(gmax5_0[gi5_0] === 92.0, "gb max of c t5");
var gc5_0 = Array.from(g5.GROUP_BY_COUNT("cat", mk5).values); assert_true(gc5_0[gi5_0] === 21, "gb count of c t5");
var gi5_1 = gb5.keys.indexOf("b"); assert_true(gi5_1 >= 0, "gb key b present t5");
var gsum5_1 = Array.from(g5.GROUP_BY_SUM("cat", "x", mk5).values); assert_true(gsum5_1[gi5_1] === 1142.0, "gb sum of b t5");
var gmean5_1 = Array.from(g5.GROUP_BY_MEAN("cat", "x", mk5).values); assert_true(gmean5_1[gi5_1] === 63.44444444444444, "gb mean of b t5");
var gmin5_1 = Array.from(g5.GROUP_BY_MIN("cat", "x", mk5).values); assert_true(gmin5_1[gi5_1] === 22.0, "gb min of b t5");
var gmax5_1 = Array.from(g5.GROUP_BY_MAX("cat", "x", mk5).values); assert_true(gmax5_1[gi5_1] === 99.0, "gb max of b t5");
var gc5_1 = Array.from(g5.GROUP_BY_COUNT("cat", mk5).values); assert_true(gc5_1[gi5_1] === 18, "gb count of b t5");
var gi5_2 = gb5.keys.indexOf("a"); assert_true(gi5_2 >= 0, "gb key a present t5");
var gsum5_2 = Array.from(g5.GROUP_BY_SUM("cat", "x", mk5).values); assert_true(gsum5_2[gi5_2] === 995.0, "gb sum of a t5");
var gmean5_2 = Array.from(g5.GROUP_BY_MEAN("cat", "x", mk5).values); assert_true(gmean5_2[gi5_2] === 41.458333333333336, "gb mean of a t5");
var gmin5_2 = Array.from(g5.GROUP_BY_MIN("cat", "x", mk5).values); assert_true(gmin5_2[gi5_2] === 2.0, "gb min of a t5");
var gmax5_2 = Array.from(g5.GROUP_BY_MAX("cat", "x", mk5).values); assert_true(gmax5_2[gi5_2] === 99.0, "gb max of a t5");
var gc5_2 = Array.from(g5.GROUP_BY_COUNT("cat", mk5).values); assert_true(gc5_2[gi5_2] === 24, "gb count of a t5");
var gi = new DataFrame({ k: new Int32Array([2,1,2,0,1]), v: new Float64Array([1,2,3,4,5]) });
var r = gi.GROUP_BY_SUM("k", "v");
assert_arr_eq(r.keys, [0,1,2], "int key sorted order");
assert_arr_close(Array.from(r.values), [4,7,4], 1e-12, 0, "int key sums");
var gf = new DataFrame({ k: new Float64Array([1.5,2.5]), v: new Float64Array([1,2]) });
assert_throws(function () { gf.GROUP_BY_SUM("k", "v"); }, null, "float key refused");
summary(__TAG); // 75 cases
