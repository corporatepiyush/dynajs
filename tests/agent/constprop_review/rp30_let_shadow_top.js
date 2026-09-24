// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
const OP = { HALT: 1 };
{ let OP = { HALT: 3 }; __A("rp30_let_shadow_top.js:block", function () { assert_eq(OP.HALT, 3, "block"); }); }
__A("rp30_let_shadow_top.js:top", function () { assert_eq(OP.HALT, 1, "top"); });
function f() { return OP.HALT; }
__A("rp30_let_shadow_top.js:fn", function () { assert_eq(f(), 1, "fn"); });

summary("constprop_review");
