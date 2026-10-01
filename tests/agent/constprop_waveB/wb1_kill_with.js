__EXP = null;
const M = Object.freeze({ S: "hello", I: 7 });
with ({}) { }

test("post", function () { assert_eq(String((function(){ return M.S + M.I; })()), "hello7", "post"); });
summary("constprop_waveB");
