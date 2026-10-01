__EXP = null;
const M = Object.freeze({ T: true, F: false });
test("chain0", function () { assert_eq(String((function(){ return (M.T === true); })()), "true", "chain0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
