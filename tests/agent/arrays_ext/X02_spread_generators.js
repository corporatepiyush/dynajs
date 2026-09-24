// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A [0,1,2,3,4] [0,0,1,2,9]"];
__EXP[1] = ["B [0,1,2,3,0,1] len=6"];
__EXP[2] = ["C 0|1|2|3"];
__EXP[3] = ["D closed at 4"];
__EXP[4] = ["E 0,1,2,3"];
__EXP[5] = ["F throw => lit=RangeError push=RangeError dst=[]"];
__EXP[6] = ["G mutate => [1,2,3] then=[1,2,3,4]"];
__EXP[7] = ["H holes => [null,\"one\",null] len=3"];
__EXP[8] = ["I custom => [0,2,4]"];
// X02: spread of generator output — lazy iterators through the bulk-append fast path,
// generator return() on break, throw mid-yield, generator over a mutating array
const out = typeof console !== "undefined" ? console.log : print;

function* nat(n) { for (let i = 0; i < n; i++) yield i; }

{
  __L(0, "A " + JSON.stringify([...nat(5)]) + " " + JSON.stringify([0, ...nat(3), 9]));
}
{
  const b = [];
  b.push(...nat(4), ...nat(2));
  __L(1, "B " + JSON.stringify(b) + " len=" + b.length);
}
{
  // spread call position with generator
  const f = (...args) => args.join("|");
  __L(2, "C " + f(...nat(4)));
}
{
  // infinite generator into a bounded for-of with break (return() must close it)
  function* inf() { let i = 0; try { while (true) yield i++; } finally { __L(3, "D closed at " + i); } }
  const got = [];
  for (const v of inf()) { got.push(v); if (v >= 3) break; }
  __L(4, "E " + got.join(","));
}
{
  // generator that THROWS mid-spread — partial results discarded, call/literal aborted
  function* boom() { yield 1; yield 2; throw new RangeError("GEN"); }
  let r1, r2;
  try { r1 = JSON.stringify([...boom()]); } catch (e) { r1 = e.name; }
  const dst = [];
  try { dst.push(...boom()); r2 = "pushed"; } catch (e) { r2 = e.name; }
  __L(5, "F throw => lit=" + r1 + " push=" + r2 + " dst=" + JSON.stringify(dst));
}
{
  // generator iterating an array that MUTATES during the spread
  const g = [1, 2, 3];
  function* overX() { for (let i = 0; i < g.length; i++) yield g[i]; }
  const h = [...overX()];
  g.push(4);
  const h2 = [...overX()];
  __L(6, "G mutate => " + JSON.stringify(h) + " then=" + JSON.stringify(h2));
}
{
  // generator + holey source: yields explicit undefined for holes (for-of over array does Get)
  function* yieldEach(arr) { yield* arr; }
  const sparse = [];
  sparse.length = 3;
  sparse[1] = "one";
  __L(7, "H holes => " + JSON.stringify([...yieldEach(sparse)]) + " len=" + [...yieldEach(sparse)].length);
}
{
  // iterator whose next() increments properly; spread through push
  const custom = { [Symbol.iterator]: () => ({ i: 0, next() { return this.i < 3 ? { value: this.i++ * 2, done: false } : { value: undefined, done: true }; } }) };
  const i2 = [];
  i2.push(...custom);
  __L(8, "I custom => " + JSON.stringify(i2));
}

summary("arrays_ext");
