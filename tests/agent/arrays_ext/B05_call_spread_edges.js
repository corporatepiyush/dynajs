// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["counts => all-match-up-to-257"];
__EXP[2] = ["holes => U,one,U,U"];
__EXP[3] = ["getters => [1,\"G\",3]:1"];
__EXP[4] = ["method => T:a|b"];
__EXP[5] = ["call => C:x"];
__EXP[6] = ["new => [1,2]"];
__EXP[7] = ["plain => [2,1]"];
__EXP[8] = ["extra => 1+2=5"];
__EXP[9] = ["nested => [1]"];
__EXP[10] = ["mid-throw => ReferenceError"];
// B05: f(...x) call-path spreads — argument-count edges, holes, getters, this-binding
const out = typeof console !== "undefined" ? console.log : print;

const count = function () { return arguments.length; };

{
  // argument counts around common inline-cache / arg-register boundaries
  for (const n of [0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257]) {
    const arr = new Array(n).fill(1);
    const got = count(...arr);
    if (got !== n) { __L(0, "MISMATCH n=" + n + " got=" + got); break; }
    if (n === 257) __L(1, "counts => all-match-up-to-257");
  }
}
{
  // holes in call position become undefined args, not skipped
  const a = [];
  a.length = 4;
  a[1] = "one";
  const f = (x, y, z, w) => [x, y, z, w].map((v) => (v === undefined ? "U" : v)).join(",");
  __L(2, "holes => " + f(...a));
}
{
  // getters in call position observed once per arg
  let calls = 0;
  const b = [1, 2, 3];
  Object.defineProperty(b, "1", { get() { calls++; return "G"; }, configurable: true });
  const f2 = (...args) => JSON.stringify(args) + ":" + calls;
  __L(3, "getters => " + f2(...b));
}
{
  // spread call with this-binding + methods
  const obj = {
    m(...args) { return this.tag + ":" + args.join("|"); },
    tag: "T",
  };
  __L(4, "method => " + obj.m(...["a", "b"]));
  __L(5, "call => " + obj.m.call({ tag: "C" }, ...["x"]));
}
{
  // new-target with spread
  function Pair(a, b) { return this && this instanceof Pair ? [a, b] : [b, a]; }
  __L(6, "new => " + JSON.stringify(new Pair(...[1, 2])));
  __L(7, "plain => " + JSON.stringify(Pair(...[1, 2])));
}
{
  // spread exceeding available formal params: extra args visible via arguments
  const g = function two(a, b) { return a + "+" + b + "=" + arguments.length; };
  __L(8, "extra => " + g(...[1, 2, 3, 4, 5]));
}
{
  // nested spread calls
  const h = (x) => [x];
  __L(9, "nested => " + JSON.stringify([...h(...[...[1, 2]])]));
}
{
  // throw inside a getter mid-call-spread: call must not happen
  const c = [1, 2];
  Object.defineProperty(c, "1", { get() { throw new ReferenceError("ARG"); }, configurable: true });
  let marker = "uncalled";
  const sink = () => { marker = "CALLED"; };
  try { sink(...c); } catch (e) { marker = e.name; }
  __L(10, "mid-throw => " + marker);
}

summary("arrays_ext");
