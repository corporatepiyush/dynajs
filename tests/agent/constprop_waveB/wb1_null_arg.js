__EXP = null;
const M = Object.freeze({ N: null });
test("arg0", function () { assert_eq(String((function(){ return [1, M.N][1]; })()), "null", "arg0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
