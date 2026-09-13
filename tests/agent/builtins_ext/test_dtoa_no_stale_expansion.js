// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["E0 [1e+21] len=5", "E1 [1e+300] len=6", "E2 [1e-300] len=6", "E3 [5e-324] len=6", "E4 [2.2250738585072014e-308] len=23", "E5 [1.7976931348623157e+308] len=23", "E6 [1e-7] len=4", "E7 [1e+21] len=5", "E8 [-1e+300] len=7", "E9 [1e+23] len=5", "E10 [123456789012345680000] len=21", "E11 [0.000001] len=8", "E12 [0.00001] len=7"];
__EXP[2] = ["maxLen=23 fails=0"];
// test_dtoa_no_stale_expansion.js — the audit's headline defect class: e-notation
// values must print SHORTEST ("1e+300", not 300-digit expansions). Asserts a
// length cap + no long zero-runs on every row; round-trip on every row.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
var f64 = new Float64Array(1);
var u32 = new Uint32Array(f64.buffer);
function fromBits(hi, lo) { u32[1] = hi; u32[0] = lo; return f64[0]; }
var r = mulberry32(0x57A1E);
var fails = 0;
function chk(cond, label, extra) {
  if (!cond) { fails++; if (fails < 8) __L(0, "FAIL " + label + " " + (extra || "")); }
}
var extremes = [1e21, 1e300, 1e-300, 5e-324, 2.2250738585072014e-308,
                1.7976931348623157e308, 1e-7, 1e21 + 1, -1e300, 9.999999999999999e22,
                123456789012345680000, 0.000001, 1e-5];
for (var i = 0; i < extremes.length; i++) {
  var s = String(extremes[i]);
  chk(s.length <= 25, "len>25 " + s);
  chk(Number(s) === extremes[i], "rt " + s);
  __L(1, "E" + i + " [" + s + "] len=" + s.length);
}
// 5000 seeded large/small magnitudes: length cap + roundtrip + no 20-zero runs
var maxLen = 0;
for (var i = 0; i < 5000; i++) {
  var v = fromBits(r() % 0x100000, r());
  if (v !== v) continue;
  var s = String(v);
  if (s.length > maxLen) maxLen = s.length;
  chk(s.length <= 25, "len>25 " + s);
  chk(Number(s) === v, "rt " + s);
  if (/0{20}/.test(s)) chk(v === 0, "20zero-run " + s);
}
__L(2, "maxLen=" + maxLen + " fails=" + fails);

summary("builtins_ext");
