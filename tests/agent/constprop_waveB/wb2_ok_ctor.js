__EXP = null;
function C() { const M = Object.freeze({ A: 4 }); this.v = M.A; }

test("f", function () { assert_eq(String((function(){ return new C().v; })()), "4", "f"); });
summary("constprop_waveB");
