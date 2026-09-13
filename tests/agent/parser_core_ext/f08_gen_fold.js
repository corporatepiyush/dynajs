// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["g1 {\"value\":5,\"done\":false} {\"value\":\"xy20\",\"done\":false}"];
__EXP[1] = ["g2 {\"value\":102,\"done\":false} {\"value\":\"mid10\",\"done\":false}"];
__EXP[2] = ["g3 {\"value\":103,\"done\":false} {\"value\":\"end64\",\"done\":true} {\"done\":true}"];
__EXP[3] = ["g4 {\"value\":101,\"done\":false} {\"value\":101,\"done\":false}"];
__EXP[5] = ["gb 255 5591810 6640386 {\"done\":true}", "gb 257 5723904 6772480 {\"done\":true}"];
__EXP[6] = ["g6 18 6"];
// F8: folding in generator bodies resumed at odd points.
function* g1() {
  var a = 2 + 3;
  yield a;
  var b = "x" + "y";
  yield b + (10 + 10);
  for (var i = 0; i < 3; i++) {
    yield i + (100 + 1);
    if (i === 1) yield "mid" + (5 + 5);
  }
  return "end" + (1 << 6);
}
var it = g1();
__L(0, "g1", JSON.stringify(it.next()), JSON.stringify(it.next()));
it.next();
__L(1, "g2", JSON.stringify(it.next()), JSON.stringify(it.next()));
__L(2, "g3", JSON.stringify(it.next()), JSON.stringify(it.next()), JSON.stringify(it.next()));
// interleaved consumption of two instances — folds independent per instance
var a1 = g1(), a2 = g1();
a1.next(); a2.next(); a2.next(); a1.next();
__L(3, "g4", JSON.stringify(a1.next()), JSON.stringify(a2.next()));
// for-of over a generator whose folds sit across yield boundaries
var out = [];
for (var v of g1()) out.push(String(v));
__A("f08_gen_fold.js:g5", function () { assert_eq(out.join(","), "5,xy20,101,102,mid10,103", "g5"); });
// generator with a large folded constant pool (cpool boundary inside gen body)
function mkgen(n) {
  var parts = ["(function*(){ var s = 0;"];
  for (var i = 0; i < n; i++) parts.push("s += " + (3 + i) + "*" + i + ";");
  parts.push("yield s; s += " + (1 << 20) + "; yield s;})");
  return eval(parts.join(""));
}
for (var n of [255, 257]) {
  var git = mkgen(n)();
  __L(5, "gb", n, git.next().value, git.next().value, JSON.stringify(git.next()));
}
// yield inside a folded default + spread consumed by the generator caller
function* g2(x = 4 * 4) { yield x + (1 + 1); }
__L(6, "g6", [...g2()].join(","), [...g2(2 * 2)].join(","));

summary("parser_core_ext");
