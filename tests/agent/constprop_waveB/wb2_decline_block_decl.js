__EXP = null;
function f() { { const M = Object.freeze({ A: 5 }); return M.A; } }

test("f", function () { assert_eq(String((function(){ return f(); })()), "5", "f"); });
summary("constprop_waveB");
