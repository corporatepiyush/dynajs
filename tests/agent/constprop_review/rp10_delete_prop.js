// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["true undefined 3"];
__EXP[1] = ["[null,3]"];
__EXP[2] = ["br-del undefined undefined"];
const OP = { HALT: 1, KEEP: 3 };
__L(0, delete OP.HALT, OP.HALT, OP.KEEP);
__L(1, (function(){ return [OP.HALT, OP.KEEP]; })());
const PQ = { X: 1 };
function g() { return PQ.X; }
delete PQ["X"];
__L(2, "br-del", g(), PQ.X);

summary("constprop_review");
