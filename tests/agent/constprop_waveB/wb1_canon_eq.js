__EXP = null;
const M = Object.freeze({ C: "7", D: "00" });
test("eq0", function () { assert_eq(String((function(){ return (M.C === M.C); })()), "true", "eq0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
