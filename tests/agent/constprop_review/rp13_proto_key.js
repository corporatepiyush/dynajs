// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1 null"];
__EXP[1] = ["1 true"];
const OP = { __proto__: null, A: 1 };
__L(0, OP.A, Object.getPrototypeOf(OP));
const B = { A: 1, __proto__: {} };
__L(1, B.A, Object.getPrototypeOf(B) !== null);

summary("constprop_review");
