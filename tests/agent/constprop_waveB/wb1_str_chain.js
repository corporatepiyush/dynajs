__EXP = null;
const M = Object.freeze({ S: "hello" });
test("chain0", function () { assert_eq(String((function(){ return M.S.length; })()), "5", "chain0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
