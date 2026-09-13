// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["4 1"];
const OP = { HALT: 1 };
function f() { var OP = { HALT: 4 }; return OP.HALT; }
__L(0, f(), OP.HALT);

summary("constprop_review");
