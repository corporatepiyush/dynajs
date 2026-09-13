// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["int-a str-1 d"];
const OP = { A: 1 };
function f(v) { switch (v) { case OP.A: return "int-a"; case "1": return "str-1"; default: return "d"; } }
__L(0, f(1), f("1"), f(2));

summary("constprop_review");
