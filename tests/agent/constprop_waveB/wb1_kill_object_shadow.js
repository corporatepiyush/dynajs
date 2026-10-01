__EXP = null;
var Object2 = Object;
function fake() { return Object2; }
const M = Object.freeze({ S: "hi" });

test("post", function () { assert_eq(String((function(){ return M.S; })()), "hi", "post"); });
test("objok", function () { assert_eq(String((function(){ return typeof fake(); })()), "function", "objok"); });
summary("constprop_waveB");
