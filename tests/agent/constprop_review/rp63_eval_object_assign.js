// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 1"];
// (d) direct eval between decl and use assigning Object.freeze
const OP = Object.freeze({ HALT: 1 });
eval("Object.freeze = function(o){ return {}; };");
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
