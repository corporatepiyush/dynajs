__EXP = null;
function f() { const A = Object.freeze({ X: 1 });
    const B = Object.freeze({ Y: 2 });
    { const C = Object.freeze({ Z: 3 }); return A.X + B.Y + C.Z; } }

test("f", function () { assert_eq(String((function(){ return f(); })()), "6", "f"); });
summary("constprop_waveB");
