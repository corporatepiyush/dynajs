__EXP = {};
__EXP[0] = ["1 2"];
__EXP[1] = ["2"];
const base = { A: 1 };
const OP = { ...base, B: 2 };
__L(0, OP.A, OP.B);
function f() { return OP.B; }
__L(1, f());

summary("constprop_review");
