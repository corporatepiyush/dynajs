// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["d1 5,56,1024 10,20,1 5,5,1024"];
__EXP[1] = ["d2 84 6 12"];
__EXP[4] = ["ar 3,7,25 2"];
__EXP[5] = ["nd 11 5 3"];
__EXP[6] = ["df xy:2:24 2:0:"];
__EXP[7] = ["gs 2,4 9"];
// F7: folding + spread + destructuring defaults.
function d1([a = 2 + 3, b = 7 * 8], extra = 1 << 10) { return a + "," + b + "," + extra; }
__L(0, "d1", d1([]), d1([10, 20], 1), d1([, 5]));
function d2(obj) {
  var { x = 40 + 2, y = 1 + 1 } = obj;
  return x * y;
}
__L(1, "d2", d2({}), d2({ x: 3 }), d2({ x: 3, y: 4 }));
function sp(a, b, c) { return a * 100 + b * 10 + c; }
__A("f07_spread_destr_fold.js:sp1", function () { assert_eq(sp(...[1 + 1, 2 + 2, 3 + 3]), 246, "sp1"); });
__A("f07_spread_destr_fold.js:sp2", function () { assert_eq(sp(...[1 + 1, 2 + 2], 9 * 9), 321, "sp2"); });
var arr = [...[1 + 2, 3 + 4], 5 * 5];
__L(4, "ar", arr.join(","), [(2 ? 1 + 1 : 9 + 9)].join(","));
// nested destructure with folded defaults inside a call with spread
function nd([x, [y = 2 + 2]] = [7, []]) { return x + y; }
__L(5, "nd", nd(), nd([1, []]), nd([1, [2]]));
// spread of a folded-length array + default param interplay
function dflt(a = "ab".length, ...rest) { return a + ":" + rest.length + ":" + rest.join(""); }
__L(6, "df", dflt(...["xy", 1 + 1, 2 + 2]), dflt());
// generator spread with folded elements
function* g() { yield 1 + 1; yield 2 + 2; }
__L(7, "gs", [...g()].join(","), Math.max(...[1 + 8, 2 + 2, 3 + 3]));

summary("parser_core_ext");
