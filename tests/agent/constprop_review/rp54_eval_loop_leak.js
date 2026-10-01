__EXP = null;
for (let i = 0; i < 50000; i++) {
    (0, eval)("const Q" + (i % 97) + " = { A" + (i % 89) + ": 1 };");
}
__A("rp54_eval_loop_leak.js:done", function () { assert_eq(typeof Q1, "undefined", "done"); });

summary("constprop_review");
