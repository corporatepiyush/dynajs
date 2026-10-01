__EXP = null;
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
function rec2(n, bf, tag) { if (n === 0) return tag; return bf(n - 1); }
var part = rec2.bind(null, 0, undefined, "tagged");
__A("p09_ptc_bound_fallback.js:b", function () { assert_eq(rec2(2, part, "direct"), "tagged", "b"); });

summary("parser_core_ext");
