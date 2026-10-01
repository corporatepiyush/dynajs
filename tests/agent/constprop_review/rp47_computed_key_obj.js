__EXP = {};
__EXP[0] = ["5 1"];
__EXP[1] = ["1"];
const key = "K";
const OP = { [key]: 5, A: 1 };
__L(0, OP.K, OP.A);
function f() { return OP.A; }
__L(1, f());

summary("constprop_review");
