__EXP = null;
const M = Object.freeze({ C: "7", D: "00" });
test("cond0", function () { assert_eq(String((function(){ return (M.C ? 1 : 0); })()), "1", "cond0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
