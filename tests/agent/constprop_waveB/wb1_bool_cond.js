__EXP = null;
const M = Object.freeze({ T: true, F: false });
test("cond0", function () { assert_eq(String((function(){ return (M.T ? 1 : 0); })()), "1", "cond0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
