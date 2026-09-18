// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["after 1 1"];
// decl NOT first statement: function-body folds must be declined
function f() { return OP.HALT; }
try { f(); __L(0, "no-throw", f()); } catch (e) { __A("rp07_tdz.js:threw", function () { assert_eq(e.constructor.name, "ReferenceError", "threw"); }); }
const OP = { HALT: 1 };
__L(2, "after", f(), OP.HALT);

summary("constprop_review");
