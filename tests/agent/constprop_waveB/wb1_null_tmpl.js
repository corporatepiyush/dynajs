__EXP = null;
const M = Object.freeze({ N: null });
test("tmpl0", function () { assert_eq(String((function(){ return "t=" + M.N; })()), "t=null", "tmpl0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
