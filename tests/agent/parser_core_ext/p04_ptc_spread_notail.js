// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[4] = ["b done2 done2"];
// P4: spread call `return f(...args)` in tail position is NOT a proper tail
// call (spread path is not OP_call) — deep chains must overflow GRACEFULLY in
// both engines. If dynajs succeeds here it is a finding (iterator close and
// frame semantics differ), reported as a divergence.
function id(x) { return x; }
function rec(n) { if (n === 0) return "done"; return rec(...[n - 1]); }
function ov(e) {
  var n = e && e.name;
  return (n === "RangeError" || n === "InternalError") ? "overflow" : "other:" + n;
}
__A("p04_ptc_spread_notail.js:a", function () { assert_eq(rec(3), "done", "a"); });
try { __L(1, "deep", rec(400000)); } catch (e) { __A("p04_ptc_spread_notail.js:deep", function () { assert_eq(ov(e), "overflow", "deep"); }); }
// moderate depth inside BOTH engines' budgets (dynajs spread-call C budget
// ~700 frames, baseline-equal; node deeper) — must be exactly correct
__A("p04_ptc_spread_notail.js:mid", function () { assert_eq(rec(500), "done", "mid"); });
// spread via arguments object (still not a tail call)
function rec2(n) { if (n === 0) return "done2"; return rec2(...[n - 1]); }
__L(4, "b", rec2(4), rec2(200));

summary("parser_core_ext");
