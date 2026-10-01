__EXP = null;
__EXP = null;
function mkParent(n) {
    var s = "";
    for (var i = 0; i < n; i++) {
        s += String.fromCodePoint(0x1F600 + (i % 8));
    }
    return s;
}
var parent = mkParent(1200);
var s = parent.slice(0, 1);
test("template", function () { assert_eq(String((function(){ return `t:${s}`; })()), "t:\ud83d", "template"); });
test("template-len", function () { assert_eq(String((function(){ return `l:${s.length}`; })()), "l:1", "template-len"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "1", "len"); });
summary("sliced_strings");

summary("sliced_strings");
