__EXP = null;
function f() { const M = Object.freeze({ A: 7 }); return function () { try { M.A = 1; } catch (e) {} return M.A; }; }
var g = f();

test("g", function () { assert_eq(String((function(){ return g(); })()), "7", "g"); });
summary("constprop_waveB");
