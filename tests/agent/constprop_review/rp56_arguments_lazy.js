__EXP = {};
__EXP[0] = ["f undefined top 1"];
const arguments = Object.freeze({ A: 1 });
function f(x) { return arguments.A; }
__L(0, "f", f(2), "top", arguments.A);

summary("constprop_review");
