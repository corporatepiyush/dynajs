__EXP = null;
__EXP = null;
function mkParent(n) {
    var s = "";
    for (var i = 0; i < n; i++) {
        s += String.fromCharCode(0x100 + ((i * 7) % 500));
    }
    return s;
}
var parent = mkParent(1200);
var s = parent.slice(100, 100 + 0);
test("template", function () { assert_eq(String((function(){ return `t:${s}`; })()), "t:", "template"); });
test("template-len", function () { assert_eq(String((function(){ return `l:${s.length}`; })()), "l:0", "template-len"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "0", "len"); });
summary("sliced_strings");

summary("sliced_strings");
