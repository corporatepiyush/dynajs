__EXP = null;
const M = Object.freeze({ N: null });
test("chain0", function () { assert_eq(String((function(){ return (M.N === null); })()), "true", "chain0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
