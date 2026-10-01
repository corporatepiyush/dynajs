__EXP = {};
__EXP[0] = ["42 42"];
const OP = { HALT: 1 };
const holder = { op: OP };
holder.op.HALT = 42;
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
