__EXP = null;
var o = { f() { const M = Object.freeze({ A: 3 }); return M.A; } };

test("f", function () { assert_eq(String((function(){ return o.f(); })()), "3", "f"); });
summary("constprop_waveB");
