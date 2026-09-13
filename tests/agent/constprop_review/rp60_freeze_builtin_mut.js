// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["direct 1 closure 1"];
// (a) mutating the builtin's property, not rebinding the variable
Object.freeze = function (o) { return o; };   // fake, unfrozen passthrough
const OP = Object.freeze({ HALT: 1 });
function get() { return OP.HALT; }
__L(0, "direct", OP.HALT, "closure", get());

summary("constprop_review");
