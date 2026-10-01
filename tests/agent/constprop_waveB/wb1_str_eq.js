__EXP = null;
const M = Object.freeze({ S: "hello" });
test("eq0", function () { assert_eq(String((function(){ return (M.S === M.S); })()), "true", "eq0"); });
test("id", function () { assert_eq(String((function(){ return 1; })()), "1", "id"); });
summary("constprop_waveB");
