__EXP = {};
__EXP[0] = ["42"];
__EXP[1] = ["42"];
const OP = { HALT: 1 };
Object.defineProperty(OP, "HALT", { value: 42 });
__L(0, OP.HALT);
__L(1, (function(){ return OP.HALT; })());

summary("constprop_review");
