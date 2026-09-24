// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["object number"];
__EXP[1] = ["object"];
const OP = { A: 1 };
__L(0, typeof OP, typeof OP.A);
function f() { return typeof OP; }
__L(1, f());

summary("constprop_review");
