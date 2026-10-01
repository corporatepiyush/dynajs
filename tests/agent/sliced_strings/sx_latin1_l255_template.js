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
var s = parent.slice(100, 100 + 255);
test("template", function () { assert_eq(String((function(){ return `t:${s}`; })()), "t:wxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopq", "template"); });
test("template-len", function () { assert_eq(String((function(){ return `l:${s.length}`; })()), "l:255", "template-len"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "255", "len"); });
summary("sliced_strings");

summary("sliced_strings");
