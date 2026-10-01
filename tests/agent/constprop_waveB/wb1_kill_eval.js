__EXP = null;
const M = Object.freeze({ S: "hello", I: 7 });
eval('1');

test("post", function () { assert_eq(String((function(){ return M.S + M.I; })()), "hello7", "post"); });
summary("constprop_waveB");
