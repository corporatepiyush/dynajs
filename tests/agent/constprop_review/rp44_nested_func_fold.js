__EXP = {};
__EXP[0] = ["1"];
__EXP[1] = ["x"];
__EXP[2] = ["2 2"];
const F = Object.freeze({ A: 1 });
function outer() { function inner() { return F.A; } return inner(); }
__L(0, outer());
const G = { B: 2 };
__L(1, "x");
function outer2() { return G.B; }
__L(2, outer2(), G.B);

summary("constprop_review");
