// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1"];
const OP = { HALT: 1 };
function get() { return OP.HALT; }
__L(0, get());
try { (0, eval)("var OP = {};"); console.log("var-ok"); } catch (e) { console.log("var threw", e.constructor.name); }
console.log(get(), OP.HALT);

summary("constprop_review");
