// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["true 5"];
__EXP[1] = ["5"];
const OP = { HALT: 1 };
__L(0, Reflect.set(OP, "HALT", 5), OP.HALT);
__L(1, (function(){ return OP.HALT; })());

summary("constprop_review");
