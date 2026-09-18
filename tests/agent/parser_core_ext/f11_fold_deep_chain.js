// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["mix -4 -4"];
__EXP[3] = ["cmp true true 3"];
__EXP[4] = ["bigstr 500 true"];
__EXP[5] = ["unary 7 number"];
// F11: very deep fold chains — the folder must not blow the C stack and must
// produce the exact float64 result; mixed precedence, comparison folding.
var parts = [];
for (var i = 0; i < 2000; i++) parts.push("+" + (i % 7));
var left = eval("(0" + parts.join("") + ")");
__A("f11_fold_deep_chain.js:left", function () { assert_eq(left, 5995, "left"); });
var depth = 400;
var rparts = [];
for (var i = 0; i < depth; i++) rparts.push("(1+");
var right = eval(rparts.join("") + "0" + ")".repeat(depth));
__A("f11_fold_deep_chain.js:right", function () { assert_eq(right, 400, "right"); });
// mixed precedence chain in one expression
__L(2, "mix", 1 + 2 * 3 % 4 - 5 << 1, ((1 + 2 * 3 % 4 - 5) << 1));
__L(3, "cmp", 1 < 2 === 2 > 1, (1 < 2) === (2 > 1), 3 & 2 | 1);
// string fold chain 500 deep
var sparts = [];
for (var i = 0; i < 500; i++) sparts.push('+"a"');
var bigstr = eval('(""' + sparts.join("") + ")");
__L(4, "bigstr", bigstr.length, bigstr === "a".repeat(500));
// deep unary fold chain (nested parenthesized unary minus, valid grammar)
var depth = 300;
var uparts = [];
for (var i = 0; i < depth; i++) uparts.push("-(");
var neg = eval(uparts.join("") + "7" + ")".repeat(depth));
__L(5, "unary", neg, typeof neg);
// right-nested comparisons folded
var cparts = [];
for (var i = 0; i < 200; i++) cparts.push("(true&&");
var cval = eval(cparts.join("") + "true" + ")".repeat(200));
__A("f11_fold_deep_chain.js:bool", function () { assert_eq(cval, true, "bool"); });

summary("parser_core_ext");
