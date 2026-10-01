__EXP = {};
__EXP[4] = ["b done2 done2"];
function id(x) { return x; }
function rec(n) { if (n === 0) return "done"; return rec(...[n - 1]); }
function ov(e) {
  var n = e && e.name;
  return (n === "RangeError" || n === "InternalError") ? "overflow" : "other:" + n;
}
__A("p04_ptc_spread_notail.js:a", function () { assert_eq(rec(3), "done", "a"); });
try { __L(1, "deep", rec(400000)); } catch (e) { __A("p04_ptc_spread_notail.js:deep", function () { assert_eq(ov(e), "overflow", "deep"); }); }
__A("p04_ptc_spread_notail.js:mid", function () { assert_eq(rec(500), "done", "mid"); });
function rec2(n) { if (n === 0) return "done2"; return rec2(...[n - 1]); }
__L(4, "b", rec2(4), rec2(200));

summary("parser_core_ext");
