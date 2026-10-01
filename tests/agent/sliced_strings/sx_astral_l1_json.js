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
test("json", function () { assert_eq(String((function(){ return JSON.stringify(s); })()), "\"\\ud83d\"", "json"); });
test("len", function () { assert_eq(String((function(){ return s.length; })()), "1", "len"); });
summary("sliced_strings");

summary("sliced_strings");
