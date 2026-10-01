__EXP = null;
const M = Object.freeze({
    A: 11,
    S: "line"
});
function f() {
    let x = M.
A;
    let y = M[
"S"];
    return x + ":" + y;
}

test("f", function () { assert_eq(String((function(){ return f(); })()), "11:line", "f"); });
test("line", function () { assert_eq(String((function(){ return 1; })()), "1", "line"); });
summary("constprop_waveB");
