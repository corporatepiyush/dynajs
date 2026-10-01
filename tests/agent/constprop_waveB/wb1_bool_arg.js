__EXP = null;
const M = Object.freeze({ T: true, F: false });
test("arg0", function () { assert_eq(String((function(){ return [1, M.T][1]; })()), "true", "arg0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
