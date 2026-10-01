__EXP = null;
function f() { const M = Object.freeze({ S: "hello" }); try { M.S = 2; } catch (e) {} return M.S; }

test("f", function () { assert_eq(String((function(){ return f(); })()), "hello", "f"); });
summary("constprop_waveB");
