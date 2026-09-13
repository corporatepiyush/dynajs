// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["warmup"];
__EXP[1] = ["1"];
__EXP[2] = ["1"];
__L(0, "warmup");
const OP = { HALT: 1 };
__L(1, OP.HALT);
function f() { return OP.HALT; }
__L(2, f());

summary("constprop_review");
