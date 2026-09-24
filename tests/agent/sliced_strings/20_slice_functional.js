// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 20_slice_functional"];
/* functional pipelines over slices (callback-heavy, exercises error paths). */
var s = parent(1000);
var t = s.slice(20, 500);
var mapped = Array.from(t).map(function (c, i) { return i % 2 ? c.toUpperCase() : c; }).join("");
eq(mapped.length, t.length, "map len");
eq(mapped, Array.from(t).map(function (c, i) { return i % 2 ? c.toUpperCase() : c; }).join(""), "map deterministic");
var reduced = t.split("").reduce(function (acc, c) { return acc + (c === "a" ? 1 : 0); }, 0);
eq(reduced, t.split("").filter(function (c) { return c === "a"; }).length, "reduce vs filter");
/* exceptions thrown from callbacks while holding slices */
var caught = 0;
try {
  t.split("").forEach(function (c, i) { if (i === 100) throw new Error("stop"); });
} catch (e) { caught = 1; }
eq(caught, 1, "throw from forEach");
eq(t.length, 480, "slice intact after throw");
/* slices crossing try/catch and generators */
function* gen(x) { yield x.slice(0, 10); yield x.slice(10, 20); }
var g = gen(t);
eq(g.next().value, s.slice(20, 30), "gen yield 1");
eq(g.next().value, s.slice(30, 40), "gen yield 2");
/* async-safe: slices through promise callbacks */
var done = false;
Promise.resolve(t.slice(0, 5)).then(function (v) {
  eq(v, s.slice(20, 25), "promise slice");
  done = true;
  __L(0, "PASS 20_slice_functional");
});

__FINISH("sliced_strings");
