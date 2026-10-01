__EXP = {};
__EXP[0] = ["undefined"];
__EXP[2] = ["1"];
const OP = { A: 1 };
function f() { return OP.A.B; }
try { __L(0, f()); } catch (e) { __L(1, "threw", e.constructor.name); }
__L(2, OP.A);

summary("constprop_review");
