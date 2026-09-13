// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// test_str_eval_built_source.js — eval of source code assembled from cached
// single-char strings (charAt/split/index/fromCharCode): the parser must see
// byte-identical text regardless of how the pieces were created.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var fails = 0;
function chk(c, l) { if (!c) { fails++; __L(0, "FAIL " + l); } }
// arithmetic expression from charAt pieces
var src = "40" + "+".charAt(0) + "2";
chk(eval(src) === 42, "eval-42");
// build "1+2*3" from split
var e2 = ["1", "+", "2", "*", "3"].join("");
chk(eval(e2) === 7, "eval-precedence");
// string literal assembled char by char, evaluated
var q = String.fromCharCode(34); // "
var e3 = q + "A".charAt(0) + "B".split("")[0] + "C" + q;
chk(eval(e3) === "ABC", "eval-string-literal");
// escape sequence built from chars
var e4 = q + "\\" + "u0041" + q;
chk(eval(e4) === "A", "eval-escape " + e4);
// variable assignment + read-back (digit extracted from a string)
eval("var evX = " + "12345".charAt(0) + ";");
chk(evX === 1, "eval-var " + evX);
// eval of a function built from chars
var e5 = "(function(" + "abc".charAt(0) + "," + "abc".charAt(1) + "){return " + "abc".charAt(0) + "*".charAt(0) + "abc".charAt(1) + "})";
var f5 = eval(e5);
chk(f5(6, 7) === 42, "eval-fn");
// wide identifier characters? keep to ASCII but exercise wide string VALUES
var e6 = q + "\u00e9".charAt(0) + q;
chk(eval(e6) === "\u00e9", "eval-wide-value");
// eval of JSON built from cached chars
var j = "{\"" + "k".charAt(0) + "\":" + "123".split("").join("") + "}";
chk(eval("(" + j + ")").k === 123, "eval-json");
console.log("fails=" + fails);

summary("builtins_ext");
