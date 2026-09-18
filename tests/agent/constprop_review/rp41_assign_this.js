// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["42 42"];
const OP = { HALT: 1 };
const holder = { op: OP };
holder.op.HALT = 42;         // alias store the parser cannot attribute
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
