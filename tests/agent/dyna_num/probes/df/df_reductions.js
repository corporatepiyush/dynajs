// GENERATED probe (df_reductions) -- do not edit; run materialize.sh
import { DataFrame } from "dyna:dataframe";
var __TAG = "df_reductions";
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
var df0 = new DataFrame({ x: new Float64Array([]), id: new Int32Array([]) });
var mk0 = new Uint8Array([]);
assert_close(df0.SUM("x", mk0), 0.0, 1e-12, 1e-12, "SUM t0");
assert_close(df0.MEAN("x", mk0), NaN, 1e-12, 1e-12, "MEAN t0");
assert_close(df0.VARIANCE_POP("x", mk0), NaN, 1e-11, 1e-12, "VARIANCE_POP t0");
assert_eq(df0.MIN("x", mk0), undefined, "MIN empty t0");
assert_eq(df0.MAX("x", mk0), undefined, "MAX empty t0");
assert_eq(df0.COUNT("x", mk0), 0, "COUNT all selected rows t0");
assert_eq(df0.SUM("id", mk0), 0, "SUM int exact t0");
var df1 = new DataFrame({ x: new Float64Array([8.986450303345919]), id: new Int32Array([7]) });
var mk1 = new Uint8Array([1]);
assert_close(df1.SUM("x", mk1), 8.986450303345919, 1e-12, 1e-12, "SUM t1");
assert_close(df1.MEAN("x", mk1), 8.986450303345919, 1e-12, 1e-12, "MEAN t1");
assert_close(df1.VARIANCE_POP("x", mk1), 0.0, 1e-11, 1e-12, "VARIANCE_POP t1");
assert_close(df1.VARIANCE("x", mk1), NaN, 1e-11, 1e-12, "VARIANCE t1");
assert_close(df1.MIN("x", mk1), 8.986450303345919, 1e-12, 0, "MIN t1");
assert_close(df1.MAX("x", mk1), 8.986450303345919, 1e-12, 0, "MAX t1");
assert_eq(df1.COUNT("x", mk1), 1, "COUNT all selected rows t1");
assert_eq(df1.SUM("id", mk1), 7, "SUM int exact t1");
var df2 = new DataFrame({ x: new Float64Array([-0.7570317853242159,6.388892666436732]), id: new Int32Array([92,83]) });
var mk2 = new Uint8Array([1,1]);
assert_close(df2.SUM("x", mk2), 5.631860881112516, 1e-12, 1e-12, "SUM t2");
assert_close(df2.MEAN("x", mk2), 2.815930440556258, 1e-12, 1e-12, "MEAN t2");
assert_close(df2.VARIANCE_POP("x", mk2), 12.76605906756875, 1e-11, 1e-12, "VARIANCE_POP t2");
assert_close(df2.VARIANCE("x", mk2), 25.5321181351375, 1e-11, 1e-12, "VARIANCE t2");
assert_close(df2.MIN("x", mk2), -0.7570317853242159, 1e-12, 0, "MIN t2");
assert_close(df2.MAX("x", mk2), 6.388892666436732, 1e-12, 0, "MAX t2");
assert_eq(df2.COUNT("x", mk2), 2, "COUNT all selected rows t2");
assert_eq(df2.SUM("id", mk2), 175, "SUM int exact t2");
var df3 = new DataFrame({ x: new Float64Array([-9.320221794769168,NaN,-9.551457110792398,5.574017739854753,-3.4002088103443384]), id: new Int32Array([77,32,47,78,81]) });
var mk3 = new Uint8Array([1,1,1,1,1]);
assert_close(df3.SUM("x", mk3), NaN, 1e-12, 1e-12, "SUM t3");
assert_close(df3.MEAN("x", mk3), NaN, 1e-12, 1e-12, "MEAN t3");
assert_close(df3.VARIANCE_POP("x", mk3), NaN, 1e-11, 1e-12, "VARIANCE_POP t3");
assert_close(df3.VARIANCE("x", mk3), NaN, 1e-11, 1e-12, "VARIANCE t3");
assert_close(df3.MIN("x", mk3), -9.551457110792398, 1e-12, 0, "MIN t3");
assert_close(df3.MAX("x", mk3), 5.574017739854753, 1e-12, 0, "MAX t3");
assert_eq(df3.COUNT("x", mk3), 5, "COUNT all selected rows t3");
assert_eq(df3.SUM("id", mk3), 315, "SUM int exact t3");
var df4 = new DataFrame({ x: new Float64Array([8.807461503893137,NaN,5.441843895241618,9.931086511351168,6.496666260063648,8.12789190094918,3.987786890938878,-4.304000507108867,8.277264069765806,-2.3029136145487428,NaN,8.111100741662085,-5.316625460982323,8.72591785620898,-6.859034290537238,-9.33109703939408,0.42186202481389046]), id: new Int32Array([11,46,45,84,35,66,25,84,95,62,81,4,47,10,65,92,95]) });
var mk4 = new Uint8Array([1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0]);
assert_close(df4.SUM("x", mk4), NaN, 1e-12, 1e-12, "SUM t4");
assert_close(df4.MEAN("x", mk4), NaN, 1e-12, 1e-12, "MEAN t4");
assert_close(df4.VARIANCE_POP("x", mk4), NaN, 1e-11, 1e-12, "VARIANCE_POP t4");
assert_close(df4.VARIANCE("x", mk4), NaN, 1e-11, 1e-12, "VARIANCE t4");
assert_close(df4.MIN("x", mk4), -9.33109703939408, 1e-12, 0, "MIN t4");
assert_close(df4.MAX("x", mk4), 9.931086511351168, 1e-12, 0, "MAX t4");
assert_eq(df4.COUNT("x", mk4), 15, "COUNT all selected rows t4");
assert_eq(df4.SUM("id", mk4), 807, "SUM int exact t4");
var df5 = new DataFrame({ x: new Float64Array([1.4474167209118605,NaN,-9.450961016118526,3.8360047666355968,-9.444456426426768,-9.111838559620082,1.642907839268446,5.892517766915262,7.857334082946181,-6.264224550686777,NaN,-4.693466951139271,-8.355485638603568,9.988762852735817,-9.791190456598997,3.4265890112146735,7.795251561328769,-4.173519774340093,-3.2810239866375923,-1.7299984814599156,-1.0009926091879606,7.498550866730511,-9.897195938974619,-5.348962270654738,3.297797879204154,-8.263758248649538,-7.4774739146232605,2.45362117420882,-1.4936406072229147,2.589621734805405,-5.160513576120138,0.8610680839046836,-5.926279099658132,5.003001005388796,4.9698542430996895,-8.644645032472908,-3.051317511126399,-9.558848212473094,2.9004926793277264,7.298417440615594,3.011700129136443,-5.121189705096185,-3.572515770792961,2.9079803032800555,-9.364323308691382,-5.534040066413581,-3.320187609642744,9.44041886832565,-2.0618407893925905,-0.8186042448505759,-2.509300448000431,-8.606848460622132,-9.712557597085834,9.78707015980035,7.679101135581732,0.5390636390075088,9.625078430399299,8.39571984950453,-9.696144014596939,-9.394537513144314,2.167292917147279,-2.035725968889892,7.952993009239435,0.4100637277588248]), id: new Int32Array([58,69,56,71,58,5,4,99,74,77,60,19,6,97,88,51,86,9,40,75,62,9,88,3,74,5,68,95,82,93,88,11,26,49,24,59,10,13,48,55,10,85,72,71,94,77,44,11,70,61,88,91,30,29,84,23,94,69,20,39,62,89,4,71]) });
var mk5 = new Uint8Array([1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0]);
assert_close(df5.SUM("x", mk5), NaN, 1e-12, 1e-12, "SUM t5");
assert_close(df5.MEAN("x", mk5), NaN, 1e-12, 1e-12, "MEAN t5");
assert_close(df5.VARIANCE_POP("x", mk5), NaN, 1e-11, 1e-12, "VARIANCE_POP t5");
assert_close(df5.VARIANCE("x", mk5), NaN, 1e-11, 1e-12, "VARIANCE t5");
assert_close(df5.MIN("x", mk5), -9.897195938974619, 1e-12, 0, "MIN t5");
assert_close(df5.MAX("x", mk5), 9.988762852735817, 1e-12, 0, "MAX t5");
assert_eq(df5.COUNT("x", mk5), 62, "COUNT all selected rows t5");
assert_eq(df5.SUM("id", mk5), 3325, "SUM int exact t5");
var df6 = new DataFrame({ x: new Float64Array([-0.739970626309514,NaN,6.971275545656681,7.1489936439320445,3.3665254432708025,-9.515180210582912,9.381338935345411,-2.087284722365439,7.118864124640822,-7.971572778187692,NaN,9.986728499643505,3.9772285614162683,1.092550870962441,2.959848213940859,6.069669364951551,1.1210554372519255,9.523051218129694,1.5502167865633965,-0.6819861056283116,1.7988884914666414,-0.41238203179091215,4.519892688840628,-0.9007480973377824,6.994638284668326,-4.987852997146547,-1.288715898990631,4.889592076651752,-7.027251785621047,8.442898583598435,0.48622364178299904,-3.8713016966357827,6.264766780659556,5.64693680498749,-7.7933187410235405,-9.156042751856148,2.6598261017352343,-8.236649702303112,-4.624366629868746,-9.143222817219794,1.7615266796201468,9.917754181660712,-5.4994117468595505,-3.616581936366856,-6.3262715842574835,7.512573269195855,-9.257732313126326,-2.1571471402421594,-0.6222521234303713,-9.494393481872976,9.41094484180212,-2.3158398689702153,-8.636538190767169,-9.01062726508826,-9.62706157937646,0.04594785626977682,6.0768169071525335,-1.6162624722346663,-4.570236951112747,-8.93969148863107,4.746245825663209,9.55432150978595,-8.267564084380865,-2.386194602586329,4.1504894476383924,-1.8308102479204535,-4.706560336053371,-2.622009781189263,3.8903253991156816,8.606322486884892,3.6588415317237377,-2.0780480885878205,1.7267028149217367,4.724367060698569,-8.196931257843971,7.719396720640361,-6.45221664570272,8.809171137399971,-9.69115985557437,6.862759529612958,-0.4726115334779024,-8.991402848623693,9.894754104316235,-9.70315356273204,3.037352906540036,-5.431881989352405,1.3530326448380947,1.3845085864886642,-6.123715499415994,7.179694049991667,4.959921836853027,-1.3831527577713132,-7.622769838199019,-6.243568765930831,8.421248439699411,3.280450119636953,-4.043251844123006,-9.054479389451444,-2.5843672081828117,0.8941589621827006]), id: new Int32Array([18,33,92,19,74,41,56,79,10,9,60,23,90,97,92,91,90,85,40,19,90,77,40,23,94,1,92,83,26,89,28,79,62,17,92,95,70,93,88,7,2,93,52,19,38,41,20,87,70,73,88,11,94,65,60,27,46,85,36,11,6,37,24,67,10,41,36,67,98,49,96,11,90,33,40,23,18,1,68,3,66,57,68,71,98,89,80,95,10,33,52,35,14,1,12,43,38,25,56,63]) });
var mk6 = new Uint8Array([1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0]);
assert_close(df6.SUM("x", mk6), NaN, 1e-12, 1e-12, "SUM t6");
assert_close(df6.MEAN("x", mk6), NaN, 1e-12, 1e-12, "MEAN t6");
assert_close(df6.VARIANCE_POP("x", mk6), NaN, 1e-11, 1e-12, "VARIANCE_POP t6");
assert_close(df6.VARIANCE("x", mk6), NaN, 1e-11, 1e-12, "VARIANCE t6");
assert_close(df6.MIN("x", mk6), -9.70315356273204, 1e-12, 0, "MIN t6");
assert_close(df6.MAX("x", mk6), 9.986728499643505, 1e-12, 0, "MAX t6");
assert_eq(df6.COUNT("x", mk6), 98, "COUNT all selected rows t6");
assert_eq(df6.SUM("id", mk6), 5051, "SUM int exact t6");
var df7 = new DataFrame({ x: new Float64Array([]), id: new Int32Array([]) });
var mk7 = new Uint8Array([]);
assert_close(df7.SUM("x", mk7), 0.0, 1e-12, 1e-12, "SUM t7");
assert_close(df7.MEAN("x", mk7), NaN, 1e-12, 1e-12, "MEAN t7");
assert_close(df7.VARIANCE_POP("x", mk7), NaN, 1e-11, 1e-12, "VARIANCE_POP t7");
assert_eq(df7.MIN("x", mk7), undefined, "MIN empty t7");
assert_eq(df7.MAX("x", mk7), undefined, "MAX empty t7");
assert_eq(df7.COUNT("x", mk7), 0, "COUNT all selected rows t7");
assert_eq(df7.SUM("id", mk7), 0, "SUM int exact t7");
var db = new DataFrame({ id: new Int32Array([2147483647,2147483647,2147483647,2147483647]) });
assert_eq(db.SUM_CHECKED("id"), 8589934588, "SUM_CHECKED big");
var dn = new DataFrame({ x: new Float64Array([NaN, NaN]) });
assert_nan(dn.SUM("x"), "SUM all-NaN is NaN");
assert_nan(dn.MEAN("x"), "MEAN all-NaN is NaN");
assert_close(dn.MIN("x"), Infinity, 0, 0, "MIN all-NaN +Inf identity");
assert_close(dn.MAX("x"), -Infinity, 0, 0, "MAX all-NaN -Inf identity");
assert_eq(dn.COUNT("x"), 2, "COUNT counts rows not non-NaN");
assert_eq(dn.COUNT_NULLS("x"), 2, "COUNT_NULLS");
summary(__TAG); // 71 cases
