__EXP = null;
var r = (function () { const M = Object.freeze({ A: 5 }); return M.A; })();

test("r", function () { assert_eq(String((function(){ return r; })()), "5", "r"); });
summary("constprop_waveB");
