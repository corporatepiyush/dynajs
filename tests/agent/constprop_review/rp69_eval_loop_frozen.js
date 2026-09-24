// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
for (let i = 0; i < 1000000; i++) {
    (0, eval)("const Q" + (i % 97) + " = Object.freeze({ A" + (i % 89) + ": 1 });");
}
__A("rp69_eval_loop_frozen.js:done", function () { assert_eq(typeof Q1, "undefined", "done"); });

summary("constprop_review");
