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
var s = parent.slice(100, 100 + 1);
test("json", function () { assert_eq(String((function(){ return JSON.stringify(s); })()), "\"w\"", "json"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "1", "len"); });
summary("sliced_strings");

summary("sliced_strings");
