// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["big a none true"];
__EXP[1] = ["big a"];
const OP = { BIG: 2147483648, A: 1 };
let hits = 0;
function t(v) { switch (v) { case OP.BIG: return "big"; case OP.A: return "a"; default: return "none"; } }
__L(0, t(2147483648), t(1), t(0), OP.BIG === 2147483648);
__L(1, t(OP.BIG), t(OP.A));

summary("constprop_review");
