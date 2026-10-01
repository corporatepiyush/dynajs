__EXP = {};
__EXP[0] = ["first"];
__EXP[1] = ["1 1"];
__L(0, "first");
const F = Object.freeze({ A: 1 });
function ff() { return F.A; }
__L(1, ff(), F.A);

summary("constprop_review");
