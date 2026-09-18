// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["2 2"];
__EXP[1] = ["7"];
__EXP[2] = ["9 9"];
const OP = { N: 1 };
OP.N++;
__L(0, OP.N, (function(){ return OP.N; })());
const PQ = { N: 5 };
PQ.N += 2;
__L(1, PQ.N);
const R = { N: 2 };
function rn() { return R.N; }
R.N = 9;
__L(2, rn(), R.N);

summary("constprop_review");
