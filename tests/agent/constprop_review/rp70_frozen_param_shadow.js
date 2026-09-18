// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["99 7 1"];
const OP = Object.freeze({ HALT: 1 });
function f(OP) { return OP.HALT; }
__L(0, f({ HALT: 99 }), f({ HALT: 7 }), OP.HALT);

summary("constprop_review");
