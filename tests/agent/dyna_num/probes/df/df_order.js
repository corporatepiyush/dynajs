// GENERATED probe (df_order) -- do not edit; run materialize.sh
import { DataFrame } from "dyna:dataframe";
var __TAG = "df_order";
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
var o0 = new DataFrame({ x: new Float64Array([3.0]) });
var om0 = new Uint8Array([1]);
assert_arr_close(Array.from(o0.SORT("x", om0)), [3.0], 0, 0, "SORT NaN-last exact t0");
assert_arr_eq(Array.from(o0.ARG_SORT("x", om0)), [0], "ARG_SORT stable t0");
var rk0 = Array.from(o0.RANK("x", om0)); var want0 = [1.0]; var rok0 = rk0.length === want0.length && rk0.every(function (v, i) { return (v !== v && want0[i] !== want0[i]) || v === want0[i]; }); assert_true(rok0, "RANK average ties t0");
var o1 = new DataFrame({ x: new Float64Array([2.0,0.0]) });
var om1 = new Uint8Array([1,1]);
assert_arr_close(Array.from(o1.SORT("x", om1)), [0.0,2.0], 0, 0, "SORT NaN-last exact t1");
assert_arr_eq(Array.from(o1.ARG_SORT("x", om1)), [1,0], "ARG_SORT stable t1");
var rk1 = Array.from(o1.RANK("x", om1)); var want1 = [2.0,1.0]; var rok1 = rk1.length === want1.length && rk1.every(function (v, i) { return (v !== v && want1[i] !== want1[i]) || v === want1[i]; }); assert_true(rok1, "RANK average ties t1");
var o2 = new DataFrame({ x: new Float64Array([8.0,6.0,8.0,NaN,2.0,4.0,1.0,6.0]) });
var om2 = new Uint8Array([1,0,1,1,1,1,1,1]);
assert_arr_close(Array.from(o2.SORT("x", om2)), [1.0,2.0,4.0,6.0,8.0,8.0,NaN], 0, 0, "SORT NaN-last exact t2");
assert_arr_eq(Array.from(o2.ARG_SORT("x", om2)), [6,4,5,7,0,2,3], "ARG_SORT stable t2");
var rk2 = Array.from(o2.RANK("x", om2)); var want2 = [5.5,NaN,5.5,NaN,2.0,3.0,1.0,4.0]; var rok2 = rk2.length === want2.length && rk2.every(function (v, i) { return (v !== v && want2[i] !== want2[i]) || v === want2[i]; }); assert_true(rok2, "RANK average ties t2");
assert_arr_close(Array.from(o2.N_LARGEST("x", 3, om2)), [8.0,8.0,6.0], 0, 0, "N_LARGEST t2");
assert_arr_close(Array.from(o2.N_SMALLEST("x", 3, om2)), [1.0,2.0,4.0], 0, 0, "N_SMALLEST t2");
var o3 = new DataFrame({ x: new Float64Array([7.0,7.0,1.0,NaN,3.0,1.0,2.0,5.0,8.0,NaN,0.0,5.0,2.0,8.0,2.0,5.0,5.0]) });
var om3 = new Uint8Array([1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]);
assert_arr_close(Array.from(o3.SORT("x", om3)), [0.0,1.0,1.0,2.0,2.0,2.0,3.0,5.0,5.0,5.0,5.0,7.0,8.0,8.0,NaN,NaN], 0, 0, "SORT NaN-last exact t3");
assert_arr_eq(Array.from(o3.ARG_SORT("x", om3)), [10,2,5,6,12,14,4,7,11,15,16,0,8,13,3,9], "ARG_SORT stable t3");
var rk3 = Array.from(o3.RANK("x", om3)); var want3 = [12.0,NaN,2.5,NaN,7.0,2.5,5.0,9.5,13.5,NaN,1.0,9.5,5.0,13.5,5.0,9.5,9.5]; var rok3 = rk3.length === want3.length && rk3.every(function (v, i) { return (v !== v && want3[i] !== want3[i]) || v === want3[i]; }); assert_true(rok3, "RANK average ties t3");
assert_arr_close(Array.from(o3.N_LARGEST("x", 3, om3)), [8.0,8.0,7.0], 0, 0, "N_LARGEST t3");
assert_arr_close(Array.from(o3.N_SMALLEST("x", 3, om3)), [0.0,1.0,1.0], 0, 0, "N_SMALLEST t3");
var o4 = new DataFrame({ x: new Float64Array([1.0,7.0,0.0,NaN,2.0,6.0,0.0,4.0,2.0,NaN,6.0,3.0,3.0,3.0,4.0,6.0,0.0,7.0,6.0,4.0,3.0,6.0,0.0,0.0,1.0,5.0,7.0,0.0,5.0,2.0,7.0,7.0,8.0,7.0,7.0,5.0,7.0,2.0,5.0,4.0]) });
var om4 = new Uint8Array([1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]);
assert_arr_close(Array.from(o4.SORT("x", om4)), [0.0,0.0,0.0,0.0,0.0,0.0,1.0,1.0,2.0,2.0,2.0,2.0,3.0,3.0,3.0,3.0,4.0,4.0,4.0,4.0,5.0,5.0,5.0,5.0,6.0,6.0,6.0,6.0,6.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,8.0,NaN,NaN], 0, 0, "SORT NaN-last exact t4");
assert_arr_eq(Array.from(o4.ARG_SORT("x", om4)), [2,6,16,22,23,27,0,24,4,8,29,37,11,12,13,20,7,14,19,39,25,28,35,38,5,10,15,18,21,17,26,30,31,33,34,36,32,3,9], "ARG_SORT stable t4");
var rk4 = Array.from(o4.RANK("x", om4)); var want4 = [7.5,NaN,3.5,NaN,10.5,27.0,3.5,18.5,10.5,NaN,27.0,14.5,14.5,14.5,18.5,27.0,3.5,33.0,27.0,18.5,14.5,27.0,3.5,3.5,7.5,22.5,33.0,3.5,22.5,10.5,33.0,33.0,37.0,33.0,33.0,22.5,33.0,10.5,22.5,18.5]; var rok4 = rk4.length === want4.length && rk4.every(function (v, i) { return (v !== v && want4[i] !== want4[i]) || v === want4[i]; }); assert_true(rok4, "RANK average ties t4");
assert_arr_close(Array.from(o4.N_LARGEST("x", 3, om4)), [8.0,7.0,7.0], 0, 0, "N_LARGEST t4");
assert_arr_close(Array.from(o4.N_SMALLEST("x", 3, om4)), [0.0,0.0,0.0], 0, 0, "N_SMALLEST t4");
var o5 = new DataFrame({ x: new Float64Array([8.0,3.0,3.0,NaN,3.0,7.0,2.0,7.0,4.0,NaN,1.0,6.0,4.0,3.0,8.0,2.0,7.0,3.0,5.0,7.0,7.0,7.0,2.0,7.0,8.0,6.0,5.0,1.0,3.0,6.0,1.0,4.0,8.0,4.0,4.0,7.0,7.0,4.0,5.0,2.0,5.0,4.0,0.0,3.0,1.0,6.0,6.0,0.0,4.0,0.0,6.0,4.0,5.0,6.0,0.0,6.0,1.0,5.0,7.0,3.0,7.0,3.0,7.0,8.0,1.0,1.0,8.0,1.0,1.0,5.0,4.0,3.0,7.0,1.0,4.0,7.0,7.0,6.0,6.0,2.0,4.0,6.0,5.0,5.0,8.0,3.0,0.0,7.0,6.0,1.0,5.0,8.0,6.0,0.0,0.0,5.0,8.0,6.0,8.0,7.0]) });
var om5 = new Uint8Array([1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]);
assert_arr_close(Array.from(o5.SORT("x", om5)), [0.0,0.0,0.0,0.0,0.0,0.0,0.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,2.0,2.0,2.0,2.0,2.0,3.0,3.0,3.0,3.0,3.0,3.0,3.0,3.0,3.0,3.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,4.0,5.0,5.0,5.0,5.0,5.0,5.0,5.0,5.0,5.0,5.0,5.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,6.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,7.0,8.0,8.0,8.0,8.0,8.0,8.0,8.0,8.0,8.0,8.0,NaN,NaN], 0, 0, "SORT NaN-last exact t5");
assert_arr_eq(Array.from(o5.ARG_SORT("x", om5)), [42,47,49,54,86,93,94,10,27,30,44,56,64,65,67,68,73,89,6,15,22,39,79,2,4,13,17,28,43,59,61,71,85,8,12,31,33,34,37,41,48,51,70,74,80,18,26,38,40,52,57,69,82,83,90,95,11,25,29,45,46,50,53,55,77,78,81,88,92,97,5,7,16,19,20,21,23,35,36,58,60,62,72,75,76,87,99,0,14,24,32,63,66,84,91,96,98,3,9], "ARG_SORT stable t5");
var rk5 = Array.from(o5.RANK("x", om5)); var want5 = [92.5,NaN,28.5,NaN,28.5,79.0,21.0,79.0,39.5,NaN,13.0,63.5,39.5,28.5,92.5,21.0,79.0,28.5,51.0,79.0,79.0,79.0,21.0,79.0,92.5,63.5,51.0,13.0,28.5,63.5,13.0,39.5,92.5,39.5,39.5,79.0,79.0,39.5,51.0,21.0,51.0,39.5,4.0,28.5,13.0,63.5,63.5,4.0,39.5,4.0,63.5,39.5,51.0,63.5,4.0,63.5,13.0,51.0,79.0,28.5,79.0,28.5,79.0,92.5,13.0,13.0,92.5,13.0,13.0,51.0,39.5,28.5,79.0,13.0,39.5,79.0,79.0,63.5,63.5,21.0,39.5,63.5,51.0,51.0,92.5,28.5,4.0,79.0,63.5,13.0,51.0,92.5,63.5,4.0,4.0,51.0,92.5,63.5,92.5,79.0]; var rok5 = rk5.length === want5.length && rk5.every(function (v, i) { return (v !== v && want5[i] !== want5[i]) || v === want5[i]; }); assert_true(rok5, "RANK average ties t5");
assert_arr_close(Array.from(o5.N_LARGEST("x", 3, om5)), [8.0,8.0,8.0], 0, 0, "N_LARGEST t5");
assert_arr_close(Array.from(o5.N_SMALLEST("x", 3, om5)), [0.0,0.0,0.0], 0, 0, "N_SMALLEST t5");
var u = new DataFrame({ s: ["b","a","b","c","a","b"], x: new Float64Array([1,2,3,4,5,6]) });
assert_arr_eq(Array.from(u.UNIQUE("s")), ["b","a","c"], "UNIQUE first-seen");
assert_eq(u.MODE("s"), "b", "MODE most frequent");
var ga = u.GROUP_ARRAY_SORTED("s", "x");
assert_arr_eq(ga.keys, ["b","a","c"], "group array keys first-seen");
assert_arr_close(Array.from(ga.values[0]), [1,3,6], 0, 0, "group b sorted");
assert_arr_close(Array.from(ga.values[1]), [2,5], 0, 0, "group a sorted");
assert_arr_close(Array.from(ga.values[2]), [4], 0, 0, "group c sorted");
var e0 = new DataFrame({ x: new Float64Array([]), s: [] });
assert_eq(e0.ROWS, 0, "0-row frame");
assert_eq(e0.SUM("x"), 0, "0-row SUM is 0");
assert_eq(Array.from(e0.ARG_SORT("x")).length, 0, "0-row ARG_SORT empty");
var rt = new DataFrame({ id: new Int32Array([1,2,3]), v: new Float64Array([3.5,1,NaN]), s: ["plain","has,comma",'has"quote'] });
var text = rt.TO_CSV();
function parseCsv(t) {
  var rows = [[]], field = "", inQ = false, i = 0;
  while (i < t.length) {
    var ch = t[i];
    if (inQ) { if (ch === '"') { if (t[i+1] === '"') { field += '"'; i += 2; continue; } inQ = false; i++; continue; } field += ch; i++; continue; }
    if (ch === '"') { inQ = true; i++; continue; }
    if (ch === ',') { rows[rows.length-1].push(field); field = ""; i++; continue; }
    if (ch === '\n' || ch === '\r') { if (ch === '\r' && t[i+1] === '\n') i++; rows[rows.length-1].push(field); field = ""; i++; if (i < t.length) rows.push([]); continue; }
    field += ch; i++;
  }
  if (field !== "" || rows[rows.length-1].length > 0) rows[rows.length-1].push(field);
  return rows.filter(function (r) { return r.length > 0; });
}
var parsed = parseCsv(text);
var header = parsed[0];
var recs = [];
for (var r = 1; r < parsed.length; r++) { var o = {}; for (var c = 0; c < header.length; c++) o[header[c]] = parsed[r][c]; recs.push(o); }
var back = new DataFrame({}).FROM_RECORDS(recs);
assert_eq(back.ROWS, 3, "csv round trip rows");
assert_eq(back.TO_COLUMNS().id[2], "3", "csv round trip id col (string; CSV has no dtypes)");
assert_true(parseFloat(back.TO_COLUMNS().v[1]) === 1, "csv round trip float col");
assert_true(isNaN(parseFloat(back.TO_COLUMNS().v[2])), "csv round trip NaN cell");
assert_eq(back.TO_COLUMNS().s[1], "has,comma", "csv round trip quoted comma");
assert_eq(back.TO_COLUMNS().s[2], 'has"quote', "csv round trip escaped quote");
summary(__TAG); // 45 cases
