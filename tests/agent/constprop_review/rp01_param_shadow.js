// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// rp01: function PARAMETER shadows the global const binding name.
// cp_shadowed() walks fd->vars only; params live in fd->args.
const OP = { HALT: 1 };
function f(OP) { return OP.HALT; }
__A("rp01_param_shadow.js:f99", function () { assert_eq(f({ HALT: 99 }), 99, "f99"); });
__A("rp01_param_shadow.js:f7", function () { assert_eq(f({ HALT: 7 }), 7, "f7"); });
__A("rp01_param_shadow.js:top", function () { assert_eq(OP.HALT, 1, "top"); });

summary("constprop_review");
