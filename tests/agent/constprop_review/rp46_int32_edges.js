// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~min max neg d", "~min max neg d"]];
const OP = { MIN: -2147483648, MAX: 2147483647, NEG: -1 };
function f(v) { switch (v) { case OP.MIN: return "min"; case OP.MAX: return "max"; case OP.NEG: return "neg"; default: return "d"; } }
__L(0, f(-2147483648), f(2147483647), f(-1), f(0), OP.MIN, OP.MAX);

summary("constprop_review");
