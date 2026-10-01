__EXP = {};
__EXP[0] = ["2 3"];
__EXP[1] = ["5"];
const OP = { A: 1, A: 2, B: 3 };
__L(0, OP.A, OP.B);
function f() { return OP.A + OP.B; }
__L(1, f());

summary("constprop_review");
