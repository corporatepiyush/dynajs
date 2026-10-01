__EXP = null;
var f = () => {
    const M = Object.freeze({ A: 2 });
    return M.A * M.A;
};

test("f", function () { assert_eq(String((function(){ return f(); })()), "4", "f"); });
summary("constprop_waveB");
