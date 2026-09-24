// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1"];
__EXP[1] = ["1 1"];
const OP = { HALT: 1 };
__L(0, OP?.HALT);
function f() { return OP?.HALT; }
__L(1, f(), OP.HALT);

summary("constprop_review");
