__EXP = null;
__EXP = null;
function mkParent(n) {
    var s = "";
    for (var i = 0; i < n; i++) {
        s += String.fromCharCode(97 + (i % 26));
    }
    return s;
}
var parent = mkParent(1200);
var s = parent.slice(100, 100 + 63);
test("template", function () { assert_eq(String((function(){ return `t:${s}`; })()), "t:wxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefg", "template"); });
test("template-len", function () { assert_eq(String((function(){ return `l:${s.length}`; })()), "l:63", "template-len"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "63", "len"); });
summary("sliced_strings");

summary("sliced_strings");
