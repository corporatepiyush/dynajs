__EXP = null;
const M = Object.freeze({ A: 2, L: 3 });
function f() { let n = 0; while (n < M.L) { n += M.A; } return n; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "4", "f"); });
summary("constprop_waveB");
