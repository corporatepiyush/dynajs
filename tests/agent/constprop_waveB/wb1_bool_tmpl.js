__EXP = null;
const M = Object.freeze({ T: true, F: false });
test("tmpl0", function () { assert_eq(String((function(){ return "t=" + M.T; })()), "t=true", "tmpl0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
