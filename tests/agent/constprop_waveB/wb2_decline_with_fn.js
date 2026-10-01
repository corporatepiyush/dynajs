__EXP = null;
function f() { with ({}) { } const M = Object.freeze({ A: 4 }); return M.A; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "4", "f"); });
summary("constprop_waveB");
