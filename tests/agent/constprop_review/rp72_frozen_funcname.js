__EXP = {};
__EXP[0] = ["undefined OP 1"];
__EXP[1] = ["undefined object 5"];
const OP = Object.freeze({ HALT: 1 });
const g = function OP() { return OP.HALT; };
__L(0, typeof g(), g.name, OP.HALT);
const C = Object.freeze({ X: 5 });
const h = class C { };
__L(1, typeof (new h()).x, typeof (new h()), C.X);

summary("constprop_review");
