// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["true undefined null str true false 3"];
__EXP[1] = ["[3,null,\"str\",null]"];
const OP = { NAN: NaN, UN: undefined, NUL: null, S: "str", T: true, F: false, A: 3 };
__L(0, OP.NAN !== OP.NAN, OP.UN, OP.NUL, OP.S, OP.T, OP.F, OP.A);
function f() { return [OP.A, OP.NUL, OP.S, OP.UN]; }
__L(1, JSON.stringify(f()));

summary("constprop_review");
