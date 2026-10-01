__EXP = null;
function f(c) { const M = Object.freeze({ A: 9 });
    if (c) { return M.A; } return -M.A; }

test("t", function () { assert_eq(String((function(){ return f(true); })()), "9", "t"); });
test("u", function () { assert_eq(String((function(){ return f(false); })()), "-9", "u"); });
summary("constprop_waveB");
