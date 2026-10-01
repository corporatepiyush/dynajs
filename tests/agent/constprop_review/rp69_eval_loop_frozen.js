__EXP = null;
for (let i = 0; i < 1000000; i++) {
    (0, eval)("const Q" + (i % 97) + " = Object.freeze({ A" + (i % 89) + ": 1 });");
}
__A("rp69_eval_loop_frozen.js:done", function () { assert_eq(typeof Q1, "undefined", "done"); });

summary("constprop_review");
