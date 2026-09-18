// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["V1 0,1,2,3", "V2 0,1,2,3,4", "V3 0,1,2", "V4 0,1,3", "V5 0,2"];
// X2: DIVERGENCE DOCUMENTATION (expected FAIL vs node — pre-existing, baseline-equal).
// for(let p ...) per-iteration binding identity when an iteration is left via
// a LABELED continue: closures created in the aborting iteration must still
// observe that iteration's own binding. node: V1 0,1,2,3 / V2 0,1,2,3,4.
// dynajs tree AND pristine baseline: V1 0,1,3,3 / V2 0,1,3,3,4 — the aborting
// iteration's closure observes the NEXT iteration's value. break (V3) and
// closure-after-continue (V4) and while-head continue (V5) are all correct.
function t(tag, build) { var fns = build(); __L(0, tag, fns.map(f => f()).join(",")); }
// V1: labeled continue to the OUTER loop head, closure before the exit
t("V1", function () {
  var fns = [];
  o1: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o1; } }
  return fns;
});
// V2: labeled continue to the SAME loop, closure before the exit
t("V2", function () {
  var fns = [];
  o2: for (let p = 0; p < 5; p++) { fns.push(() => p); if (p === 2) continue o2; }
  return fns;
});
// V3 control: break instead of continue — identity holds
t("V3", function () {
  var fns = [];
  o3: for (let p = 0; p < 4; p++) { fns.push(() => p); for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) break o3; } }
  return fns;
});
// V4 control: closure created AFTER the continue point — never created in the
// aborting iteration, both engines agree (0,1,3)
t("V4", function () {
  var fns = [];
  o4: for (let p = 0; p < 4; p++) { for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue o4; } fns.push(() => p); }
  return fns;
});
// V5 control: while-loop head + labeled continue, let captured in body — agrees
t("V5", function () {
  var fns = [];
  var p = 0;
  o5: while (p < 4) { let c = p; fns.push(() => c); for (let q = 0; q < 2; q++) { p++; if (q === 1) continue o5; } }
  return fns;
});

summary("parser_core_ext");
