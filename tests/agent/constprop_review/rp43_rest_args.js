// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["7 1"];
const OP = { A: 1 };
function f(...OP) { return OP.length + OP[0]; }
__L(0, f(5, 6), OP.A);

summary("constprop_review");
