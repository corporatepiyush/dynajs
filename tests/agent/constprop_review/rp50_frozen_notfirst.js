// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["first"];
__EXP[1] = ["1 1"];
__L(0, "first");
const F = Object.freeze({ A: 1 });
function ff() { return F.A; }
__L(1, ff(), F.A);

summary("constprop_review");
