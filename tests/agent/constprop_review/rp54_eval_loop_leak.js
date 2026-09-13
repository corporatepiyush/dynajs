// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// rp54: many separately-compiled scripts each declaring a const object.
// Patched leaks cp_binds/cp_sites (success path never frees them).
for (let i = 0; i < 50000; i++) {
    (0, eval)("const Q" + (i % 97) + " = { A" + (i % 89) + ": 1 };");
}
__A("rp54_eval_loop_leak.js:done", function () { assert_eq(typeof Q1, "undefined", "done"); });

summary("constprop_review");
