__EXP = null;
const M = Object.freeze({ W: "\u00e9\u4e2d" });
test("stmt0", function () { assert_eq(String((function(){ return M.W; })()), "\u00e9\u4e2d", "stmt0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
