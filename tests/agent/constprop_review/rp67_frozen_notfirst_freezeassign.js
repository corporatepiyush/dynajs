// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 1"];
// fake-freeze assigned INSIDE a function after decl (decl still first)
const OP = Object.freeze({ HALT: 1 });
function scramble() { Object.freeze = function (o) { return o; }; }
scramble();
__L(0, OP.HALT, (function(){ return OP.HALT; })());

summary("constprop_review");
