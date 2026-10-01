__EXP = null;
const M = Object.freeze({ C: "7", D: "00" });
test("arg0", function () { assert_eq(String((function(){ return [1, M.C][1]; })()), "7", "arg0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
