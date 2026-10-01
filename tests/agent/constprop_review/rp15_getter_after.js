__EXP = {};
__EXP[0] = ["9 9"];
const OP = { HALT: 1 };
Object.defineProperty(OP, "HALT", { get() { return 9; } });
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
