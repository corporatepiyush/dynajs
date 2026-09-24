// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~sqrt(0000000000000000)", "~sqrt(0000000000000000)"], ["~sqrt(8000000000000000)", "~sqrt(8000000000000000)"], "sqrt(7ff0000000000000) = 7ff0000000000000", "sqrt(fff0000000000000) = 7ff8000000000000", "[NAN] sqrt(7ff8000000000000) = 7ff8000000000000", "[NAN] sqrt(fff8000000000000) = fff8000000000000", "[NAN] sqrt(7ff0000000000001) = 7ff8000000000001", "[NAN] sqrt(7ff00000deadbeef) = 7ff80000deadbeef", "[NAN] sqrt(ffffffffffffffff) = ffffffffffffffff", "sqrt(0000000000000001) = 1e60000000000000", ["~abs(0000000000000000)", "~abs(0000000000000000)"], ["~abs(8000000000000000)", "~abs(8000000000000000)"], "abs(7ff0000000000000) = 7ff0000000000000", "abs(fff0000000000000) = 7ff0000000000000", "[NAN] abs(7ff8000000000000) = 7ff8000000000000", "[NAN] abs(fff8000000000000) = 7ff8000000000000", "[NAN] abs(7ff0000000000001) = 7ff0000000000001", "[NAN] abs(7ff00000deadbeef) = 7ff00000deadbeef", "[NAN] abs(ffffffffffffffff) = 7fffffffffffffff", ["~abs(0000000000000001)", "~abs(0000000000000001)"], ["~floor(0000000000000000)", "~floor(0000000000000000)"], ["~floor(8000000000000000)", "~floor(8000000000000000)"], "floor(7ff0000000000000) = 7ff0000000000000", "floor(fff0000000000000) = fff0000000000000", "[NAN] floor(7ff8000000000000) = 7ff8000000000000", "[NAN] floor(fff8000000000000) = fff8000000000000", "[NAN] floor(7ff0000000000001) = 7ff8000000000001", "[NAN] floor(7ff00000deadbeef) = 7ff80000deadbeef", "[NAN] floor(ffffffffffffffff) = ffffffffffffffff", ["~floor(0000000000000001)", "~floor(0000000000000001)"], ["~ceil(0000000000000000)", "~ceil(0000000000000000)"], ["~ceil(8000000000000000)", "~ceil(8000000000000000)"], "ceil(7ff0000000000000) = 7ff0000000000000", "ceil(fff0000000000000) = fff0000000000000", "[NAN] ceil(7ff8000000000000) = 7ff8000000000000", "[NAN] ceil(fff8000000000000) = fff8000000000000", "[NAN] ceil(7ff0000000000001) = 7ff8000000000001", "[NAN] ceil(7ff00000deadbeef) = 7ff80000deadbeef", "[NAN] ceil(ffffffffffffffff) = ffffffffffffffff", "ceil(0000000000000001) = 3ff0000000000000", ["~trunc(0000000000000000)", "~trunc(0000000000000000)"], ["~trunc(8000000000000000)", "~trunc(8000000000000000)"], "trunc(7ff0000000000000) = 7ff0000000000000", "trunc(fff0000000000000) = fff0000000000000", "[NAN] trunc(7ff8000000000000) = 7ff8000000000000", "[NAN] trunc(fff8000000000000) = fff8000000000000", "[NAN] trunc(7ff0000000000001) = 7ff8000000000001", "[NAN] trunc(7ff00000deadbeef) = 7ff80000deadbeef", "[NAN] trunc(ffffffffffffffff) = ffffffffffffffff", ["~trunc(0000000000000001)", "~trunc(0000000000000001)"], ["~neg(0000000000000000)", "~neg(0000000000000000)"], ["~neg(8000000000000000)", "~neg(8000000000000000)"], "neg(7ff0000000000000) = fff0000000000000", "neg(fff0000000000000) = 7ff0000000000000", "[NAN] neg(7ff8000000000000) = fff8000000000000", "[NAN] neg(fff8000000000000) = 7ff8000000000000", "[NAN] neg(7ff0000000000001) = fff0000000000001", "[NAN] neg(7ff00000deadbeef) = fff00000deadbeef", "[NAN] neg(ffffffffffffffff) = 7fffffffffffffff", ["~neg(0000000000000001)", "~neg(0000000000000001)"]];
// test_math_specials_nan.js — Math.* on ±0, ±Inf, quiet/signaling NaN bit
// patterns: NaN payload propagation is impl-defined ([NAN] tag vs node);
// everything else (zero signs, infinities) must be bit-identical everywhere.
var f64 = new Float64Array(1);
var u32 = new Uint32Array(f64.buffer);
function bits(v) { f64[0] = v; return u32[1].toString(16).padStart(8, "0") + u32[0].toString(16).padStart(8, "0"); }
function fromBits(hi, lo) { u32[1] = hi; u32[0] = lo; return f64[0]; }
var fns = [Math.sqrt, Math.abs, Math.floor, Math.ceil, Math.trunc,
           function (x) { return -x; }];
var names = ["sqrt", "abs", "floor", "ceil", "trunc", "neg"];
var vals = [
  [0x00000000, 0x00000000], // +0
  [0x80000000, 0x00000000], // -0
  [0x7ff00000, 0x00000000], // +Inf
  [0xfff00000, 0x00000000], // -Inf
  [0x7ff80000, 0x00000000], // canonical qNaN
  [0xfff80000, 0x00000000], // negative qNaN
  [0x7ff00000, 0x00000001], // sNaN (payload 1)
  [0x7ff00000, 0xdeadbeef], // sNaN (payload)
  [0xffffffff, 0xffffffff], // negative sNaN all-ones
  [0x00000000, 0x00000001]  // smallest denormal
];
for (var f = 0; f < fns.length; f++) {
  for (var i = 0; i < vals.length; i++) {
    var v = fromBits(vals[i][0], vals[i][1]);
    var tag = (vals[i][0] & 0x7ff00000) === 0x7ff00000 && (vals[i][0] & 0x000fffff) !== 0 || (vals[i][1] !== 0 && (vals[i][0] & 0x7ff00000) === 0x7ff00000) ? "[NAN] " : "";
    __L(0, tag + names[f] + "(" + bits(v) + ") = " + bits(fns[f](v)));
  }
}

summary("builtins_ext");
