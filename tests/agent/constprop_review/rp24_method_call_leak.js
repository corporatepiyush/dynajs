// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1"];
__EXP[1] = ["1"];
const OP = { A: 1 };
__L(0, Object.keys(OP).length);
function f() { return OP.A; }
__L(1, f());

summary("constprop_review");
