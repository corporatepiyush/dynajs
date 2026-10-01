__EXP = null;
function f() { const M = Object.freeze({ A: 1 }); return M.A; }
function g() { const M = Object.freeze({ A: 2 }); return M.A; }

test("fg", function () { assert_eq(String((function(){ return f() + "|" + g(); })()), "1|2", "fg"); });
summary("constprop_waveB");
