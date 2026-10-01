__EXP = null;
function* g() { const M = Object.freeze({ A: 6 }); yield M.A; }
var it = g();

test("v", function () { assert_eq(String((function(){ return it.next().value; })()), "6", "v"); });
summary("constprop_waveB");
