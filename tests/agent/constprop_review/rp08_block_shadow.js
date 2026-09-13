// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["outer 1 1"];
const OP = { HALT: 1 };
{ const OP = { HALT: 2 }; __A("rp08_block_shadow.js:inner", function () { assert_eq(OP.HALT, 2, "inner"); }); }
__L(1, "outer", OP.HALT, (function(){ return OP.HALT; })());
let x = 5, r = 0;
switch (x) { case OP.HALT: r = 1; break; default: r = 2; }
__A("rp08_block_shadow.js:sw", function () { assert_eq(r, 2, "sw"); });

summary("constprop_review");
