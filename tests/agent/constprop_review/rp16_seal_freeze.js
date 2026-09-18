// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 20 1"];
__EXP[2] = ["1"];
const F = Object.freeze({ A: 1 });
const S = Object.seal({ A: 2 });
function ff() { return F.A; }
function ss() { return S.A; }
S.A = 20;
__L(0, ff(), ss(), F.A);
try { "use strict"; F.A = 10; } catch (e) { __L(1, "froze threw", e.constructor.name); }
__L(2, ff());

summary("constprop_review");
