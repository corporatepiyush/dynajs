__EXP = {};
__EXP[0] = ["A B A"];
const OP = { A: 1, B: 2 };
function sw(OP, v) { switch (v) { case OP.A: return "A"; case OP.B: return "B"; default: return "D"; } }
__L(0, sw({A:1,B:2}, 1), sw({A:1,B:2}, 2), sw({A:9,B:9}, 9));

summary("constprop_review");
