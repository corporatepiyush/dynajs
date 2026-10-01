__EXP = {};
__EXP[0] = ["1"];
__EXP[1] = ["7 7"];
const OP = { HALT: 1 };
function get() { return OP.HALT; }
__L(0, get());
eval("OP.HALT = 7;");
__L(1, get(), OP.HALT);

summary("constprop_review");
