__EXP = null;
const M = Object.freeze({ T: true, F: false });
test("eq0", function () { assert_eq(String((function(){ return (M.T === M.T); })()), "true", "eq0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
