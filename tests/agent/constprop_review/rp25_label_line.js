// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["3 4"];
__EXP[1] = ["5"];
const OP = { A: 1,
B: 2 };
function f(x) { return OP.A + OP.B + x; }
__L(0, f(0), f(1));
const PQ = {C: 5};
function g() { return PQ
    .C; }
__L(1, g());

summary("constprop_review");
