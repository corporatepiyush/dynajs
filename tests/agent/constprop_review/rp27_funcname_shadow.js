__EXP = {};
__EXP[0] = ["undefined OP 1"];
const OP = { HALT: 1 };
const g = function OP() { return OP.HALT; };
__L(0, typeof g(), g.name, OP.HALT);

summary("constprop_review");
