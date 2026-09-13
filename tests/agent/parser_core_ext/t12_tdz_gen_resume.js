// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a 1,1,11,12 1,1,11,12,21,23"];
__EXP[1] = ["b {\"value\":\"q!\",\"done\":false} {\"done\":true} {\"done\":true}"];
__EXP[2] = ["c 2 2 12 102 22 202"];
// T12: generators + per-iteration loop bindings + eval, resumed at odd points.
function* outer() {
  for (let i = 0; i < 3; i++) {
    let j = i * (2 + 8);
    eval("j = j + 1");            // mutation through eval on the per-iteration copy
    yield j;
    yield (() => i + j)();
  }
}
var it = outer();
var vals = [];
vals.push(it.next().value, it.next().value, it.next().value, it.next().value);
__L(0, "a", vals.join(","), [...outer()].join(","));
// generator with for-of inside, per-iteration const capture, resumed late
function* innerGen() {
  for (const v of ["p", "q"]) {
    const cl = () => v + "!";
    yield cl();
  }
}
var g = innerGen();
g.next();
__L(1, "b", JSON.stringify(g.next()), JSON.stringify(g.next()), JSON.stringify(g.next()));
// two generators interleaved, each holding its own loop-iteration scope
function* pairGen(n) {
  for (let k = 0; k < 3; k++) {
    yield k * n + (1 + 1);
  }
}
var p1 = pairGen(10), p2 = pairGen(100);
__L(2, "c", p1.next().value, p2.next().value, p1.next().value, p2.next().value, [...p1].join(","), [...p2].join(","));

summary("parser_core_ext");
