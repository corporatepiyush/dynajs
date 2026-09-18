// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[5] = ["w6 7"];
__EXP[6] = ["w7 ab"];
__EXP[7] = ["w8 computed-dup {\"ab\":\"computed-dup\"}"];
// F2: folded constants inside with(){} — pure folds must not touch the with
// scope, dynamic names must still resolve through it (sloppy mode file).
var o = { a: 1, b: 2 };
with (o) {
  __A("f02_with_fold.js:w1", function () { assert_eq(2 + 3 * 4, 14, "w1"); });
  __A("f02_with_fold.js:w2", function () { assert_eq(a + (1 << 4), 17, "w2"); });
  __A("f02_with_fold.js:w3", function () { assert_eq((10 + 5) - a, 14, "w3"); });
}
var a = 100;
with (o) { __A("f02_with_fold.js:w4", function () { assert_eq(a * 2 + (2 + 2), 6, "w4"); }); }
__A("f02_with_fold.js:w5", function () { assert_eq(JSON.stringify({ [(2 + 3)]: "v", x: (4 + 1) }), "{\"5\":\"v\",\"x\":5}", "w5"); });
with (o) { __L(5, "w6", eval("a + (2*3)")); }
// folded string constant used as a with-scope property lookup key must stay dynamic
var shadow = { "ab": "with-hit" };
shadow[("a" + "b")] = "computed-dup";
var key = "a" + "b";
with (shadow) { __L(6, "w7", eval("key")); }
__L(7, "w8", shadow[key], JSON.stringify(shadow));
// nested with + fold: inner with shadows, fold result identical
with (o) {
  with ({ a: 50 + 50 }) {
    __A("f02_with_fold.js:w9", function () { assert_eq(a + (1 * 1), 101, "w9"); });
  }
  __A("f02_with_fold.js:w10", function () { assert_eq(a, 1, "w10"); });
}
// fold inside the with-object EXPRESSION itself (evaluated once)
var counter = 0;
function mkobj() { counter++; return { z: 9 * 9 }; }
with (mkobj()) { __A("f02_with_fold.js:w11", function () { assert_eq(z, 81, "w11"); }); }
__A("f02_with_fold.js:w12", function () { assert_eq(counter, 1, "w12"); });

summary("parser_core_ext");
