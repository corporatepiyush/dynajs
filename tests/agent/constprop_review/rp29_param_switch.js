// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A B A"];
// param shadow feeding a switch case-label expr inside the function
const OP = { A: 1, B: 2 };
function sw(OP, v) { switch (v) { case OP.A: return "A"; case OP.B: return "B"; default: return "D"; } }
__L(0, sw({A:1,B:2}, 1), sw({A:1,B:2}, 2), sw({A:9,B:9}, 9));

summary("constprop_review");
