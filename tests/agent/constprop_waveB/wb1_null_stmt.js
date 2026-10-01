__EXP = null;
const M = Object.freeze({ N: null });
test("stmt0", function () { assert_eq(String((function(){ return M.N; })()), "null", "stmt0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
