__EXP = null;
async function f() { const M = Object.freeze({ A: 6 }); return M.A; }

test("t", function () { assert_eq(String((function(){ return typeof f(); })()), "object", "t"); });
summary("constprop_waveB");
