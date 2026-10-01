__EXP = {};
__EXP[0] = ["7 1"];
const OP = { A: 1 };
function f(...OP) { return OP.length + OP[0]; }
__L(0, f(5, 6), OP.A);

summary("constprop_review");
