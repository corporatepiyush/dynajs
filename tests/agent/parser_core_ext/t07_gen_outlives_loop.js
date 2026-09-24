// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["c 0,0,10 0,0,10,11,20,22"];
// T7: loop var captured by generators that OUTLIVE the loop (and vice versa).
function* tick(v) { yield v; yield v * 2; }
var gens = [];
for (let i = 0; i < 3; i++) {
  gens.push(tick(i + (1 + 2)));
}
__A("t07_gen_outlives_loop.js:a", function () { assert_eq(gens.map(g => [...g].join("/")).join(" "), "3/6 4/8 5/10", "a"); });
// generator CLOSURE over the loop var, created in-loop, resumed after the loop
var lazy = [];
for (let j = 0; j < 3; j++) {
  lazy.push(function* () { yield j; yield j + 100; });
}
__A("t07_gen_outlives_loop.js:b", function () { assert_eq(lazy.map(f => [...f()].join("/")).join(" "), "0/100 1/101 2/102", "b"); });
// generator that yields INSIDE the loop and is consumed by for-of mid-flight
function* outer() {
  for (let i = 0; i < 3; i++) {
    let j = i * (2 + 8);
    yield j;
    yield (() => i + j)();
  }
}
var it = outer();
var vals = [];
vals.push(it.next().value, it.next().value, it.next().value);
__L(2, "c", vals.join(","), [...outer()].join(","));
// generator suspended in one iteration, loop continues, generator resumed later
var saved = [];
for (let k = 0; k < 3; k++) {
  if (k === 1) saved.push(function* () { yield k * 11; yield k * 22; });
}
__A("t07_gen_outlives_loop.js:d", function () { assert_eq([...saved[0]()].join(","), "11,22", "d"); });

summary("parser_core_ext");
