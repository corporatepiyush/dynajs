__EXP = null;
const M = Object.freeze({ S: "hello" });
function f(M) { return M.S; }

test("param", function () { assert_eq(String((function(){ return f({S: "shadow"}); })()), "shadow", "param"); });
test("top", function () { assert_eq(String((function(){ return M.S; })()), "hello", "top"); });
summary("constprop_waveB");
