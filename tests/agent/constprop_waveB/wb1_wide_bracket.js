__EXP = null;
const M = Object.freeze({ W: "\u00e9\u4e2d" });
test("bracket0", function () { assert_eq(String((function(){ return M["W"]; })()), "\u00e9\u4e2d", "bracket0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
