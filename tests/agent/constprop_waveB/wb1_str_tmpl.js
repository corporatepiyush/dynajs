__EXP = null;
const M = Object.freeze({ S: "hello" });
test("tmpl0", function () { assert_eq(String((function(){ return "t=" + M.S; })()), "t=hello", "tmpl0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
