__EXP = null;
const M = Object.freeze({ S: "hello" });
test("arg0", function () { assert_eq(String((function(){ return [1, M.S][1]; })()), "hello", "arg0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
