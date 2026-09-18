// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["2 1"];
__EXP[1] = ["99 1 99"];
__EXP[2] = ["[1,99]"];
const OP = { A: 1, B: 2 };
const k = "B";
__L(0, OP[k], OP.A);
OP[k] = 99;
__L(1, OP[k], OP.A, OP.B);
function f() { return [OP.A, OP.B]; }
__L(2, JSON.stringify(f()));

summary("constprop_review");
