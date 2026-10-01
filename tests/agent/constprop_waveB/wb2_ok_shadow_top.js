__EXP = null;
const T = Object.freeze({ A: 1 });
function f() { const T = Object.freeze({ A: 100 }); return T.A; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "100", "f"); });
test("top", function () { assert_eq(String((function(){ return T.A; })()), "1", "top"); });
summary("constprop_waveB");
