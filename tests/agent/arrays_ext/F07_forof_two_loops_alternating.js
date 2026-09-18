// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["alt => A1 A2 B3 B4 C5 C6"];
__EXP[1] = ["manual => p:false [q] r:false [] undefined:true"];
__EXP[2] = ["three => 10 20 30 left=40:true"];
__EXP[3] = ["two-iters => 1 a 2 b"];
// F07: one iterator handed to two (three) for-of loops alternately — shared cursor state
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [1, 2, 3, 4, 5, 6];
  const it = a[Symbol.iterator]();
  const w = () => ({ [Symbol.iterator]: () => it });
  const log = [];
  for (const x of w()) { log.push("A" + x); if (x === 2) break; }
  for (const y of w()) { log.push("B" + y); if (y === 4) break; }
  for (const z of w()) log.push("C" + z);
  __L(0, "alt => " + log.join(" "));
}
// alternation with an EXPLICIT manual next() between loops
{
  const eit = ["p", "q", "r"][Symbol.iterator]();
  const r1 = eit.next();
  const looped = [];
  for (const v of { [Symbol.iterator]: () => eit }) { looped.push(v); break; }
  const r2 = eit.next();
  const looped2 = [];
  for (const v of { [Symbol.iterator]: () => eit }) looped2.push(v);
  const r3 = eit.next();
  __L(1, "manual => " + r1.value + ":" + r1.done + " [" + looped.join(",") + "] " +
      r2.value + ":" + r2.done + " [" + looped2.join(",") + "] " + r3.value + ":" + r3.done);
}
// three loops, break in each; then verify exhaustion by hand
{
  const a = [10, 20, 30, 40];
  const it = a[Symbol.iterator]();
  const w = () => ({ [Symbol.iterator]: () => it });
  const log = [];
  for (const x of w()) { log.push(x); break; }
  for (const x of w()) { log.push(x); break; }
  for (const x of w()) { log.push(x); break; }
  log.push("left=" + it.next().value + ":" + it.next().done);
  __L(2, "three => " + log.join(" "));
}
// interleaved loops over TWO DIFFERENT arrays' iterators (state must not cross)
{
  const i1 = [1, 2, 3][Symbol.iterator]();
  const i2 = ["a", "b", "c"][Symbol.iterator]();
  const log = [];
  for (const x of { [Symbol.iterator]: () => i1 }) { log.push(x); break; }
  for (const y of { [Symbol.iterator]: () => i2 }) { log.push(y); break; }
  for (const x of { [Symbol.iterator]: () => i1 }) { log.push(x); break; }
  for (const y of { [Symbol.iterator]: () => i2 }) { log.push(y); break; }
  __L(3, "two-iters => " + log.join(" "));
}

summary("arrays_ext");
