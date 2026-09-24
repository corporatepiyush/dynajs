// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// P9: bound-function callee is a documented trampoline FALLBACK (not a tail
// call). Bounded depths must be exact in both engines. NOTE: dynajs's
// C-recursion budget with bound trampolines is ~450 frames (baseline-equal),
// so the bounded check stays at 300 and deep overflow is duck-typed.
function rec(n, bf) {
  if (n === 0) return "done";
  return bf(n - 1, bf);
}
var recBound = rec.bind(null);
function ov(e) {
  var n = e && e.name;
  return (n === "RangeError" || n === "InternalError") ? "overflow" : "other:" + n;
}
__A("p09_ptc_bound_fallback.js:a", function () { assert_eq(rec(3, recBound), "done", "a"); });
__A("p09_ptc_bound_fallback.js:mid", function () { assert_eq(rec(300, recBound), "done", "mid"); });
try { __L(2, "deep", rec(150000, recBound)); } catch (e) { __A("p09_ptc_bound_fallback.js:deep", function () { assert_eq(ov(e), "overflow", "deep"); }); }
// bound callee with preset args
function rec2(n, bf, tag) { if (n === 0) return tag; return bf(n - 1); }
var part = rec2.bind(null, 0, undefined, "tagged");
__A("p09_ptc_bound_fallback.js:b", function () { assert_eq(rec2(2, part, "direct"), "tagged", "b"); });

summary("parser_core_ext");
