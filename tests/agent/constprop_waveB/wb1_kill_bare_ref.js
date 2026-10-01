__EXP = null;
const M = Object.freeze({ S: "hello", I: 42 });
void M;

test("post", function () { assert_eq(String((function(){ return M.S; })()), "hello", "post"); });
test("postI", function () { assert_eq(String((function(){ return M.I; })()), "42", "postI"); });
test("still", function () { assert_eq(String((function(){ return typeof M; })()), "object", "still"); });
summary("constprop_waveB");
