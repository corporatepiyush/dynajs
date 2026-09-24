// GENERATED probe (mathx_special) -- do not edit; run materialize.sh
import { erfinv, erfcinv, erfcx, expint, psi, polygamma,
         gammainc, gammaincinv, betainc, betaincinv, betaln, beta,
         gamma, gammaln, besseli, besselk, besseliScaled, besselkScaled,
         ellipke, ellipj, legendreP, legendre, airy } from "dyna:mathx";
var __TAG = "mathx_special";
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
assert_close(erfcx(1), 0.42758357615580705, 1e-14, 0, "erfcx(1)");
assert_close(erfcx(5), 0.11070463773306864, 1e-12, 0, "erfcx(5)");
assert_close(expint(1), 0.2193839344488781, 1e-09, 0, "expint(1)");
assert_close(expint(0.5), 0.5597735950445695, 1e-09, 0, "expint(0.5)");
assert_close(expint(2), 0.04890051071517467, 1e-09, 0, "expint(2)");
assert_close(expint(5), 0.0011482955913772372, 1e-09, 0, "expint(5)");
assert_close(expint(10), 4.156968929945428e-06, 1e-08, 0, "expint(10)");
assert_close(psi(1), -0.5772156649015329, 1e-15, 0, "psi(1)");
assert_close(psi(0.5), -1.9635100260214235, 1e-15, 0, "psi(0.5)");
assert_close(psi(2), 0.42278433509846713, 1e-14, 0, "psi(2)");
assert_close(besseli(0,1), 1.2660658777520082, 1e-14, 0, "besseli(0,1)");
assert_close(besseli(1,1), 0.565159103992485, 1e-14, 0, "besseli(1,1)");
assert_close(besseli(0,2), 2.279585302336067, 1e-13, 0, "besseli(0,2)");
assert_close(besseli(0,5), 27.23987182360445, 1e-12, 0, "besseli(0,5)");
assert_close(besselk(0,1), 0.4210244382407083, 1e-10, 0, "besselk(0,1)");
assert_close(besselk(1,1), 0.6019072301972346, 1e-10, 0, "besselk(1,1)");
assert_close(besselk(0,2), 0.11389387274953343, 1e-10, 0, "besselk(0,2)");
assert_close(besselk(0,5), 0.003691098334042594, 1e-09, 0, "besselk(0,5)");
assert_close(ellipke(0.5)[0], 1.8540746773013705, 1e-13, 0, "ellipke(0.5).0");
assert_close(ellipke(0.5)[1], 1.350643881047675, 1e-13, 0, "ellipke(0.5).1");
assert_close(ellipke(0)[0], 1.5707963267948966, 1e-15, 0, "ellipke(0).0");
assert_close(ellipke(0)[1], 1.5707963267948966, 1e-15, 0, "ellipke(0).1");
assert_close(ellipke(0.9)[0], 2.578092113348172, 1e-10, 0, "ellipke(0.9).0");
assert_close(ellipke(0.9)[1], 1.1047747327040738, 1e-10, 0, "ellipke(0.9).1");
assert_close(airy(0).ai, 0.3550280538878172, 1e-14, 0, "airy(0).ai");
assert_close(airy(0).aip, -0.2588194037928078, 1e-14, 0, "airy(0).aip");
assert_close(airy(0).bi, 0.6149266274460007, 1e-14, 0, "airy(0).bi");
assert_close(airy(0).bip, 0.4482883573538264, 1e-14, 0, "airy(0).bip");
assert_close(airy(1).ai, 0.1352924163128814, 1e-13, 0, "airy(1).ai");
assert_close(airy(1).bi, 1.2074235949528713, 1e-13, 0, "airy(1).bi");
assert_close(polygamma(1,1), 1.6449340668482264, 1e-14, 0, "polygamma(1,1)");
assert_close(polygamma(2,1), -2.4041138063191885, 1e-14, 0, "polygamma(2,1)");
assert_close(polygamma(0,0.5), -1.9635100260214235, 1e-14, 0, "polygamma(0,0.5)");
assert_close(legendreP(2,0,0.5), -0.125, 1e-15, 0, "legendreP(2,0,0.5)");
assert_close(legendreP(1,0,0.5), 0.5, 1e-15, 0, "legendreP(1,0,0.5)");
assert_close(legendreP(3,0,0.5), -0.4375, 1e-15, 0, "legendreP(3,0,0.5)");
assert_close(legendreP(2,2,0.5), 2.25, 1e-15, 0, "legendreP(2,2,0.5)");
assert_close(gammainc(1,1), 0.6321205588285577, 1e-15, 0, "gammainc(1,1)");
assert_close(gammainc(1,0.5), 0.8427007929497148, 1e-15, 0, "gammainc(1,0.5)");
assert_close(betainc(0.5,1,1), 0.5, 1e-15, 0, "betainc(0.5,1,1)");
assert_close(betainc(0.3,2,1), 0.09, 1e-15, 0, "betainc(0.3,2,1)");
assert_close(betainc(0.3,1,1), 0.3, 1e-15, 0, "betainc(0.3,1,1)");
assert_close(erfcinv(0.1), 1.163087153676674, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.25), 0.8134198475976184, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.5), 0.47693627620446977, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.75), 0.22531205501217805, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.9), 0.08885599049425763, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.01), 1.8213863677184499, 1e-13, 0, "erfcinv ref");
assert_close(erfcinv(0.99), 0.008862501280950556, 1e-13, 0, "erfcinv ref");
assert_nan(erfcinv(-0.1), "erfcinv(-0.1) is NaN");
assert_nan(erfinv(1.5), "erfinv(1.5) is NaN");
assert_close(erfcx(0.05), 0.9459900435549615, 1e-13, 0, "erfcx identity");
assert_close(erfcx(0.2), 0.8090195199015807, 1e-13, 0, "erfcx identity");
assert_close(erfcx(0.45), 0.6422516980377038, 1e-13, 0, "erfcx identity");
assert_close(erfcx(0.8), 0.4891005892231148, 1e-13, 0, "erfcx identity");
assert_close(erfcx(1.4), 0.33874354067973456, 1e-13, 0, "erfcx identity");
assert_close(erfcx(2.2), 0.235592963678614, 1e-13, 0, "erfcx identity");
assert_close(erfcx(3.5), 0.15529365560889435, 1e-13, 0, "erfcx identity");
assert_close(erfcx(6.0), 0.09277656780053835, 1e-13, 0, "erfcx identity");
assert_close(erfcx(10.0), 0.05614099274382259, 1e-13, 0, "erfcx identity");
assert_close(erfcx(18.0), 0.031295717815905205, 1e-13, 0, "erfcx identity");
assert_close(erfcx(24.0), 0.02348754606368264, 1e-13, 0, "erfcx identity");
assert_close(erfcx(30.0), 0.018795888909599746, 1e-13, 0, "erfcx identity");
assert_close(erfcx(45.0), 0.012534452903720577, 1e-13, 0, "erfcx identity");
assert_close(gammaln(0.05) + gammaln(0.95), 2.9998479962897036, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.1) + gammaln(0.9), 2.319088891468949, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.25) + gammaln(0.75), 1.4913034761293729, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.5) + gammaln(0.5), 1.1447298858494002, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.75) + gammaln(0.25), 1.4913034761293729, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.9) + gammaln(0.09999999999999998), 2.3190888914689487, 1e-12, 1e-15, "lgamma reflection");
assert_close(gammaln(0.99) + gammaln(0.010000000000000009), 4.605334684806733, 1e-12, 1e-15, "lgamma reflection");
assert_close(gamma(0.5) * 0.5, gamma(1.5), 1e-13, 1e-300, "gamma recurrence");
assert_close(gamma(1.3) * 1.3, gamma(2.3), 1e-13, 1e-300, "gamma recurrence");
assert_close(gamma(2.7) * 2.7, gamma(3.7), 1e-13, 1e-300, "gamma recurrence");
assert_close(gamma(5.1) * 5.1, gamma(6.1), 1e-13, 1e-300, "gamma recurrence");
assert_close(gamma(10.4) * 10.4, gamma(11.4), 1e-13, 1e-300, "gamma recurrence");
assert_close(gamma(20.9) * 20.9, gamma(21.9), 1e-13, 1e-300, "gamma recurrence");
assert_close(beta(0.5,0.5), 3.1415926535897927, 1e-12, 1e-300, "beta product");
assert_close(betaln(0.5,0.5), 1.1447298858494008, 1e-12, 0, "betaln identity");
assert_close(beta(1.5,2.5), 0.19634954084936204, 1e-12, 1e-300, "beta product");
assert_close(betaln(1.5,2.5), -1.6278588363903808, 1e-12, 0, "betaln identity");
assert_close(beta(3.0,4.0), 0.016666666666666666, 1e-12, 1e-300, "beta product");
assert_close(betaln(3.0,4.0), -4.094344562222101, 1e-12, 0, "betaln identity");
assert_close(beta(0.3,7.7), 1.6440751216456106, 1e-12, 1e-300, "beta product");
assert_close(betaln(0.3,7.7), 0.4971779900216653, 1e-12, 0, "betaln identity");
assert_close(psi(0.1), -10.42375494041828, 1e-9, 1e-11, "psi ref");
assert_close(psi(0.5), -1.9635100260248224, 1e-9, 1e-11, "psi ref");
assert_close(psi(1.0), -0.577215664910292, 1e-9, 1e-11, "psi ref");
assert_close(psi(1.5), 0.03648997397517695, 1e-9, 1e-11, "psi ref");
assert_close(psi(2.0), 0.42278433508970803, 1e-9, 1e-11, "psi ref");
assert_close(psi(4.7), 1.4374238096294076, 1e-9, 1e-11, "psi ref");
assert_close(psi(10.0), 2.2517525890667014, 1e-9, 1e-11, "psi ref");
assert_close(psi(55.5), 4.0073469585404435, 1e-9, 1e-11, "psi ref");
assert_close(psi(-0.5), 0.03648997397517714, 1e-9, 1e-11, "psi ref");
assert_close(psi(-2.3), 3.3173231575569058, 1e-9, 1e-11, "psi ref");
assert_close(polygamma(1,0.5) - polygamma(1,1.5), 4.0, 1e-9, 1e-12, "polygamma recurrence");
assert_close(polygamma(1,2.0) - polygamma(1,3.0), 0.25, 1e-9, 1e-12, "polygamma recurrence");
assert_close(polygamma(2,1.0) - polygamma(2,2.0), -2.0, 1e-9, 1e-12, "polygamma recurrence");
assert_close(polygamma(3,0.7) - polygamma(3,1.7), 24.98958767180342, 1e-9, 1e-12, "polygamma recurrence");
assert_close(polygamma(5,1.3) - polygamma(5,2.3), 24.8611453239604, 1e-9, 1e-12, "polygamma recurrence");
assert_close(gammainc(0.3,0.5), 0.561421973919, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(0.3,0.5,"upper"), 0.43857802608100005, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(2.5,1.0), 0.9179150013761012, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(2.5,1.0,"upper"), 0.08208499862389884, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(1.0,2.0), 0.2642411176571153, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(1.0,2.0,"upper"), 0.7357588823428847, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(4.2,3.5), 0.7013536570294737, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(4.2,3.5,"upper"), 0.2986463429705263, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(7.0,9.0), 0.27090873226191736, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(7.0,9.0,"upper"), 0.7290912677380826, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(0.05,0.1), 0.7755386354510307, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(0.05,0.1,"upper"), 0.22446136454896926, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(15.0,5.0), 0.9991433587892247, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(15.0,5.0,"upper"), 0.000856641210775333, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammainc(0.0,2.5), 0.0, 1e-12, 1e-15, "gammainc P ref");
assert_close(gammainc(0.0,2.5,"upper"), 1.0, 1e-12, 1e-15, "gammainc Q ref");
assert_close(gammaincinv(0.4,0.5), 0.13749794886422811, 1e-8, 1e-10, "gammaincinv ref");
assert_close(gammaincinv(0.7,1.0), 1.2039728043259355, 1e-8, 1e-10, "gammaincinv ref");
assert_close(gammaincinv(0.3,2.0), 1.0973492107034915, 1e-8, 1e-10, "gammaincinv ref");
assert_close(gammaincinv(0.6,3.5), 3.641603816420152, 1e-8, 1e-10, "gammaincinv ref");
assert_close(gammaincinv(0.45,9.0), 8.305391096270807, 1e-8, 1e-10, "gammaincinv ref");
assert_close(betainc(0.3,0.5,0.5), 0.36901011956554497, 1e-12, 1e-15, "betainc ref");
assert_close(betainc(0.7,0.5,0.5), 0.630989880434455, 1e-12, 1e-15, "betainc symmetry");
assert_close(betainc(0.4,2.0,3.0), 0.5247999999999995, 1e-12, 1e-15, "betainc ref");
assert_close(betainc(0.6,3.0,2.0), 0.4752000000000005, 1e-12, 1e-15, "betainc symmetry");
assert_close(betainc(0.7,5.0,1.0), 0.16807000000000016, 1e-12, 1e-15, "betainc ref");
assert_close(betainc(0.30000000000000004,1.0,5.0), 0.8319299999999998, 1e-12, 1e-15, "betainc symmetry");
assert_close(betainc(0.5,3.0,3.0), 0.49999999999999933, 1e-12, 1e-15, "betainc ref");
assert_close(betainc(0.5,3.0,3.0), 0.5000000000000007, 1e-12, 1e-15, "betainc symmetry");
assert_close(betainc(0.2,0.3,4.0), 0.8893580558283087, 1e-12, 1e-15, "betainc ref");
assert_close(betainc(0.8,4.0,0.3), 0.11064194417169126, 1e-12, 1e-15, "betainc symmetry");
assert_close(betaincinv(0.4,0.5,0.5), 0.34549150281252694, 1e-8, 1e-10, "betaincinv ref");
assert_close(betaincinv(0.3,2.0,3.0), 0.2723839420751055, 1e-8, 1e-10, "betaincinv ref");
assert_close(betaincinv(0.8,5.0,1.0), 0.956352499790037, 1e-8, 1e-10, "betaincinv ref");
assert_close(betaincinv(0.5,3.0,3.0), 0.5000000000000002, 1e-8, 1e-10, "betaincinv ref");
assert_close(besseliScaled(0.5,0.5) * Math.exp(0.5), besseli(0.5,0.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(0.5,0.5) * Math.exp(-0.5), besselk(0.5,0.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(-0.5,0.5) - besseli(1.5,0.5), 1.1759861735808326, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(1.5,0.5) - besselk(-0.5,0.5), 2.1500952069998402, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(0.5,1.0) * Math.exp(1.0), besseli(0.5,1.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(0.5,1.0) * Math.exp(-1.0), besselk(0.5,1.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(-0.5,1.0) - besseli(1.5,1.0), 0.937674888245488, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(1.5,1.0) - besselk(-0.5,1.0), 0.46106850444789454, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(0.5,2.5) * Math.exp(2.5), besseli(0.5,2.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(0.5,2.5) * Math.exp(-2.5), besselk(0.5,2.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(-0.5,2.5) - besseli(1.5,2.5), 1.2212374152786873, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(1.5,2.5) - besselk(-0.5,2.5), 0.026026377261603995, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(0.5,6.0) * Math.exp(6.0), besseli(0.5,6.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(0.5,6.0) * Math.exp(-6.0), besselk(0.5,6.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(-0.5,6.0) - besseli(1.5,6.0), 10.950839486109716, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(1.5,6.0) - besselk(-0.5,6.0), 0.00021138110873026477, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.0,0.5) * Math.exp(0.5), besseli(1.0,0.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.0,0.5) * Math.exp(-0.5), besselk(1.0,0.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.0,0.5) - besseli(2.0,0.5), 1.031577221563585, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.0,0.5) - besselk(0.0,0.5), 6.625764480013183, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.0,1.0) * Math.exp(1.0), besseli(1.0,1.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.0,1.0) * Math.exp(-1.0), besselk(1.0,1.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.0,1.0) - besseli(2.0,1.0), 1.13031820798497, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.0,1.0) - besselk(0.0,1.0), 1.20381446039447, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.0,2.5) * Math.exp(2.5), besseli(1.0,2.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.0,2.5) * Math.exp(-2.5), besselk(1.0,2.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.0,2.5) - besseli(2.0,2.5), 2.013372996230958, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.0,2.5) - besselk(0.0,2.5), 0.059112653078197575, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.0,6.0) * Math.exp(6.0), besseli(1.0,6.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.0,6.0) * Math.exp(-6.0), besselk(1.0,6.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.0,6.0) - besseli(2.0,6.0), 20.447312259213412, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.0,6.0) - besselk(0.0,6.0), 0.0004479732392451697, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.5,0.5) * Math.exp(0.5), besseli(1.5,0.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.5,0.5) * Math.exp(-0.5), besselk(1.5,0.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.5,0.5) - besseli(2.5,0.5), 0.5784208430041005, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.5,0.5) - besselk(0.5,0.5), 19.350856862998562, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.5,1.0) * Math.exp(1.0), besseli(1.5,1.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.5,1.0) * Math.exp(-1.0), besselk(1.5,1.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.5,1.0) - besseli(2.5,1.0), 0.8805759790424396, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.5,1.0) - besselk(0.5,1.0), 2.766411026687367, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.5,2.5) * Math.exp(2.5), besseli(1.5,2.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.5,2.5) * Math.exp(-2.5), besselk(1.5,2.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.5,2.5) - besseli(2.5,2.5), 2.2479340666051426, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.5,2.5) - besselk(0.5,2.5), 0.10931078449873677, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(1.5,6.0) * Math.exp(6.0), besseli(1.5,6.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(1.5,6.0) * Math.exp(-6.0), besselk(1.5,6.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(0.5,6.0) - besseli(2.5,6.0), 27.377502423454246, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(2.5,6.0) - besselk(0.5,6.0), 0.0007398338805559267, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(2.0,0.5) * Math.exp(0.5), besseli(2.0,0.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(2.0,0.5) * Math.exp(-0.5), besselk(2.0,0.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(1.0,0.5) - besseli(3.0,0.5), 0.255249193421906, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(3.0,0.5) - besselk(1.0,0.5), 60.40146840992679, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(2.0,1.0) * Math.exp(1.0), besseli(2.0,1.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(2.0,1.0) * Math.exp(-1.0), besselk(2.0,1.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(1.0,1.0) - besseli(3.0,1.0), 0.5429906790681532, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(3.0,1.0) - besselk(1.0,1.0), 6.499355594540717, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(2.0,2.5) * Math.exp(2.5), besseli(2.0,2.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(2.0,2.5) * Math.exp(-2.5), besselk(2.0,2.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(1.0,2.5) - besseli(3.0,2.5), 2.042345836510663, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(3.0,2.5) - besselk(1.0,2.5), 0.19433633004570194, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(2.0,6.0) * Math.exp(6.0), besseli(2.0,6.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(2.0,6.0) * Math.exp(-6.0), besselk(2.0,6.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(1.0,6.0) - besseli(3.0,6.0), 31.19139647817636, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(3.0,6.0) - besselk(1.0,6.0), 0.0011279783781721945, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(3.0,0.5) * Math.exp(0.5), besseli(3.0,0.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(3.0,0.5) * Math.exp(-0.5), besselk(3.0,0.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(2.0,0.5) - besseli(4.0,0.5), 0.031741343627883424, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(4.0,0.5) - besselk(2.0,0.5), 744.694914359161, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(3.0,1.0) * Math.exp(1.0), besseli(3.0,1.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(3.0,1.0) * Math.exp(-1.0), besselk(3.0,1.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(2.0,1.0) - besseli(4.0,1.0), 0.1330105495459914, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(4.0,1.0) - besselk(2.0,1.0), 42.60757694842771, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(3.0,2.5) * Math.exp(2.5), besseli(3.0,2.5), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(3.0,2.5) * Math.exp(-2.5), besselk(3.0,2.5), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(2.0,2.5) - besseli(4.0,2.5), 1.1384889810672854, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(4.0,2.5) - besselk(2.0,2.5), 0.6437451513442773, 1e-9, 1e-12, "K recurrence");
assert_close(besseliScaled(3.0,6.0) * Math.exp(6.0), besseli(3.0,6.0), 1e-12, 1e-300, "I scaled pair");
assert_close(besselkScaled(3.0,6.0) * Math.exp(-6.0), besselk(3.0,6.0), 1e-11, 1e-300, "K scaled pair");
assert_close(besseli(2.0,6.0) - besseli(4.0,6.0), 30.15054029946387, 1e-9, 1e-12, "I recurrence");
assert_close(besselk(4.0,6.0) - besselk(2.0,6.0), 0.002471898095907704, 1e-9, 1e-12, "K recurrence");
var e_ = ellipj(0.0,0.5); assert_close(e_.sn*e_.sn + e_.cn*e_.cn, 1.0, 1e-12, 1e-15, "sn^2+cn^2"); assert_close(e_.dn*e_.dn + 0.5*e_.sn*e_.sn, 1.0, 1e-12, 1e-15, "dn^2+m sn^2");
var e_ = ellipj(0.5,0.5); assert_close(e_.sn*e_.sn + e_.cn*e_.cn, 1.0, 1e-12, 1e-15, "sn^2+cn^2"); assert_close(e_.dn*e_.dn + 0.5*e_.sn*e_.sn, 1.0, 1e-12, 1e-15, "dn^2+m sn^2");
var e_ = ellipj(1.0,0.25); assert_close(e_.sn*e_.sn + e_.cn*e_.cn, 1.0, 1e-12, 1e-15, "sn^2+cn^2"); assert_close(e_.dn*e_.dn + 0.25*e_.sn*e_.sn, 1.0, 1e-12, 1e-15, "dn^2+m sn^2");
var e_ = ellipj(1.5,0.9); assert_close(e_.sn*e_.sn + e_.cn*e_.cn, 1.0, 1e-12, 1e-15, "sn^2+cn^2"); assert_close(e_.dn*e_.dn + 0.9*e_.sn*e_.sn, 1.0, 1e-12, 1e-15, "dn^2+m sn^2");
assert_close(legendreP(2,0,0.5), -0.125, 1e-10, 1e-14, "legendreP ref");
assert_close(legendreP(5,0,-0.3), -0.34538625, 1e-10, 1e-14, "legendreP ref");
assert_close(legendreP(7,0,0.77), -0.11713014158128957, 1e-10, 1e-14, "legendreP ref");
assert_close(legendreP(10,2,0.4), -12.818955995999994, 1e-10, 1e-14, "legendreP ref");
assert_close(legendreP(6,3,-0.6), 46.44863999999999, 1e-10, 1e-14, "legendreP ref");
assert_close(legendre(2,0.5)[0], -0.125, 1e-10, 1e-14, "legendre m=0");
assert_close(legendre(5,-0.3)[0], -0.34538625, 1e-10, 1e-14, "legendre m=0");
assert_close(legendre(7,0.77)[0], -0.11713014158128943, 1e-10, 1e-14, "legendre m=0");
var a0 = airy(0.5), ap = airy(0.5001), am = airy(0.4999); assert_close((ap.ai - am.ai) / (2 * 0.0001), a0.aip, 1e-5, 1e-7, "Ai ODE d"); assert_close((ap.aip - am.aip) / (2 * 0.0001), 0.5 * a0.ai, 1e-4, 1e-7, "Ai ODE");
var a0 = airy(1.0), ap = airy(1.0001), am = airy(0.9999); assert_close((ap.ai - am.ai) / (2 * 0.0001), a0.aip, 1e-5, 1e-7, "Ai ODE d"); assert_close((ap.aip - am.aip) / (2 * 0.0001), 1.0 * a0.ai, 1e-4, 1e-7, "Ai ODE");
var a0 = airy(2.0), ap = airy(2.0001), am = airy(1.9999); assert_close((ap.ai - am.ai) / (2 * 0.0001), a0.aip, 1e-5, 1e-7, "Ai ODE d"); assert_close((ap.aip - am.aip) / (2 * 0.0001), 2.0 * a0.ai, 1e-4, 1e-7, "Ai ODE");
var a0 = airy(-0.5), ap = airy(-0.4999), am = airy(-0.5001); assert_close((ap.ai - am.ai) / (2 * 0.0001), a0.aip, 1e-5, 1e-7, "Ai ODE d"); assert_close((ap.aip - am.aip) / (2 * 0.0001), -0.5 * a0.ai, 1e-4, 1e-7, "Ai ODE");
summary(__TAG); // 247 cases
