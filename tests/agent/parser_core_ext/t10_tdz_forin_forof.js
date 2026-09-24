// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["d x {\"x\":1,\"z\":3}"];
// T10: for-in / for-of heads — per-iteration bindings, closure capture before
// and after mutation of the iteration variable.
var out = [];
for (const k in { a: 1, b: 2 }) {
  out.push(k);
  for (const v of [k + (1 + 1)]) out.push(v);
}
__A("t10_tdz_forin_forof.js:a", function () { assert_eq(out.join(","), "a,a2,b,b2", "a"); });
var fns = [];
for (let v of [1, 2, 3]) { v += (10 + 10); fns.push(() => v); }
__A("t10_tdz_forin_forof.js:b", function () { assert_eq(fns.map(f => f()).join(","), "21,22,23", "b"); });
var fns2 = [];
for (let w of [1, 2, 3]) { fns2.push(() => w); w = w + 5; }
__A("t10_tdz_forin_forof.js:c", function () { assert_eq(fns2.map(f => f()).join(","), "6,7,8", "c"); });
// for-in over an object mutated during iteration (keys snapshot semantics)
var obj = { x: 1, y: 2 };
var keys = [];
for (var kk in obj) { keys.push(kk); delete obj.y; obj.z = 3; }
__L(3, "d", keys.join(","), JSON.stringify(obj));
// nested for-of reusing the name + generators capturing each level
var gs = [];
for (let q of [1, 2]) {
  for (let q of [10, 20]) {
    gs.push(function* () { yield q; }());
  }
}
__A("t10_tdz_forin_forof.js:e", function () { assert_eq(gs.map(g => [...g][0]).join(","), "10,20,10,20", "e"); });

summary("parser_core_ext");
