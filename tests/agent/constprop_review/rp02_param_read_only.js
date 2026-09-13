// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// rp02: param shadow, read via bracket form too
const OP = { A: 11, B: 22 };
function g(OP, k) { return OP[k] + OP.A; }
__A("rp02_param_read_only.js:g", function () { assert_eq(g({ A: 100, B: 200 }, "B"), 300, "g"); });
function h(OP) { switch (OP.A) { case 11: return "lit-eq"; default: return "neq"; } }
__A("rp02_param_read_only.js:h", function () { assert_eq(h({ A: 11 }), "lit-eq", "h"); });

summary("constprop_review");
