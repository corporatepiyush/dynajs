__EXP = null;
const M = Object.freeze({ N: null });
test("cond0", function () { assert_eq(String((function(){ return (M.N ? 1 : 0); })()), "0", "cond0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
