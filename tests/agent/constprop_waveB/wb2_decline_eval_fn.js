__EXP = null;
function f() { eval('1'); const M = Object.freeze({ A: 3 }); return M.A; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "3", "f"); });
summary("constprop_waveB");
