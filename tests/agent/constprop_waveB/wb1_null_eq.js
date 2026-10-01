__EXP = null;
const M = Object.freeze({ N: null });
test("eq0", function () { assert_eq(String((function(){ return (M.N === M.N); })()), "true", "eq0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
