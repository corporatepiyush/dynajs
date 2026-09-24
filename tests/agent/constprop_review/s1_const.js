// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
const OP = { HALT: 1 };
globalThis.getOP = function () { return OP.HALT; };
globalThis.readOP = function () { __L(0, "read", OP.HALT); };

summary("constprop_review");
