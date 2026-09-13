// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 1"];
__EXP[1] = ["2"];
// (b) computed-member freeze form
const OP = Object["freeze"]({ HALT: 1 });
__L(0, OP.HALT, (function(){ return OP.HALT; })());
const PQ = Object[`freeze`]({ A: 2 });
__L(1, PQ.A);

summary("constprop_review");
