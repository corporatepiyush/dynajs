__EXP = null;
const M = Object.freeze({ S: "hello" });
function f() { let M = { S: "shadow" }; return M.S; }

test("let", function () { assert_eq(String((function(){ return f(); })()), "shadow", "let"); });
test("top", function () { assert_eq(String((function(){ return M.S; })()), "hello", "top"); });
summary("constprop_waveB");
