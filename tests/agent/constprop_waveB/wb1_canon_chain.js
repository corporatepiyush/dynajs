__EXP = null;
const M = Object.freeze({ C: "7", D: "00" });
test("chain0", function () { assert_eq(String((function(){ return M.C.length; })()), "1", "chain0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
