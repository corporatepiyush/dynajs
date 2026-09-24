// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["9 9"];
const OP = { HALT: 1 };
Object.defineProperty(OP, "HALT", { get() { return 9; } });
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
