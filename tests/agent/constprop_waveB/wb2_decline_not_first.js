__EXP = null;
function f() { var t = 1; const M = Object.freeze({ A: 8 }); return t + M.A; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "9", "f"); });
summary("constprop_waveB");
