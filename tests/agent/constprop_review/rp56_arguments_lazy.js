// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["f undefined top 1"];
// rp56: `arguments` is a phase-2-lazy binding (like funcexpr self-name was)
const arguments = Object.freeze({ A: 1 });
function f(x) { return arguments.A; }
__L(0, "f", f(2), "top", arguments.A);

summary("constprop_review");
