// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["s1 abcdef 6 true"];
__EXP[1] = ["s2 true a12 3a"];
__EXP[2] = ["s3 true 97"];
__EXP[3] = ["s4 2 d800 d801"];
__EXP[4] = ["s5 2 true 4"];
__EXP[5] = ["s6 312 abcd"];
__EXP[6] = [["~s7 1", "~s7 1"]];
__EXP[7] = ["s8 11 11 true"];
__EXP[8] = ["s9 true 40"];
__EXP[10] = ["s11 h h2 d"];
// F10: string constant folding — chains, comparisons, escapes, lone surrogates
// (inspected via charCodeAt so terminal encoding cannot perturb the diff).
var s1 = "ab" + "cd" + "ef";
__L(0, "s1", s1, s1.length, s1 === "abcdef");
__L(1, "s2", ("a" + "b") === ("a" + "b"), "a" + 1 + 2, 1 + 2 + "a");
__L(2, "s3", "\u0041\u0042" === "AB", "A".charCodeAt(0) + (2 << 4));
var lone = "\uD800" + "\uD801";
__L(3, "s4", lone.length, lone.charCodeAt(0).toString(16), lone.charCodeAt(1).toString(16));
var pair = "\uD800" + "\uDC00";
__L(4, "s5", pair.length, pair === "\uD800\uDC00", JSON.stringify(pair).length);
__L(5, "s6", `${1 + 2}${3 * 4}`, `a${"b" + "c"}d`);
__L(6, "s7", ("x" + "").length, ("" + 12).length, String(12) + String(34));
// folded strings used as computed member keys and property names
var obj = {};
obj["k" + "1"] = 11;
__L(7, "s8", obj.k1, obj["k" + "1"], ("k" + "1") in obj);
// long folded chain crossing many cpool slots, compared for equality
var parts = [];
for (var i = 0; i < 8; i++) parts.push("part" + i);
var joined = parts.join("");
__L(8, "s9", joined === "part0part1part2part3part4part5part6part7", joined.length);
// compare folded vs runtime-built strings
var dyn = ["p", "a", "r", "t", "0"].join("") + ["part1"].join("");
__A("f10_string_fold.js:s10", function () { assert_eq(dyn === "part0" + "part1", true, "s10"); });
// folded string in a switch label vs runtime discriminant (fused probe shape)
function sw(v) { switch (v) { case "he" + "llo": return "h"; case "hell" + "o!": return "h2"; default: return "d"; } }
__L(10, "s11", sw("hello"), sw("hello!"), sw("hellO"));

summary("parser_core_ext");
