// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["push-empty => [] len=0"];
__EXP[1] = ["push-mixed => [1,2,3]"];
__EXP[2] = ["unshift-empty => [1,2,3]"];
__EXP[3] = ["unshift-mixed => [0,1,2]"];
__EXP[4] = ["push-triple => [] len=0"];
__EXP[5] = ["empty-sparse => [null,null,null,null] len=4 own0=true"];
__EXP[6] = ["push-empty-sparse => [null,null,null,null] len=4"];
__EXP[7] = ["interleaved => len=34 [0,3,6,9,12,15,18,21,24,27,30,33,36,39,42,45,48,51,54,57,60,63,66,69,72,75,78,81,84,87,90,93,96,99]"];
__EXP[8] = ["nested => [[],[1],[1]] lens=0,1,1"];
__EXP[9] = ["ctors => [] [] klen=0"];
__EXP[10] = ["empty-iter => [] [] 00"];
// B04: expand_fast_array size-0 early-return territory — empty spreads in EVERY bulk context
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [];
  a.push(...[]);
  __L(0, "push-empty => " + JSON.stringify(a) + " len=" + a.length);
}
{
  const b = [1, 2];
  b.push(...[], 3, ...[]);
  __L(1, "push-mixed => " + JSON.stringify(b));
}
{
  const c = [1, 2, 3];
  c.unshift(...[]);
  __L(2, "unshift-empty => " + JSON.stringify(c));
}
{
  const d = [1, 2];
  d.unshift(...[], 0, ...[]);
  __L(3, "unshift-mixed => " + JSON.stringify(d));
}
{
  const e = [];
  e.push(...[], ...[], ...[]);
  __L(4, "push-triple => " + JSON.stringify(e) + " len=" + e.length);
}
{
  // spread of an EMPTY sparse array (length>0, count 0) — length>count but all holes
  const holey = [];
  holey.length = 4;
  const f = [...holey];
  __L(5, "empty-sparse => " + JSON.stringify(f) + " len=" + f.length +
      " own0=" + Object.prototype.hasOwnProperty.call(f, 0));
  const g = [];
  g.push(...holey);
  __L(6, "push-empty-sparse => " + JSON.stringify(g) + " len=" + g.length);
}
{
  // empty spread between real spreads at high iteration count
  const h = [];
  for (let i = 0; i < 100; i++) h.push(...(i % 3 ? [] : [i]));
  __L(7, "interleaved => len=" + h.length + " " + JSON.stringify(h));
}
{
  // empty-spread literal in nested position
  const i2 = [[...[], ...[]], [...[], 1], [1, ...[]]];
  __L(8, "nested => " + JSON.stringify(i2) + " lens=" + i2.map((x) => x.length).join(","));
}
{
  // constructor positions
  const j = Array.of(...[]);
  const k = new Array(...[]);
  __L(9, "ctors => " + JSON.stringify(j) + " " + JSON.stringify(k) + " klen=" + k.length);
}
{
  // empty iterable (non-array) through the bulk path
  const emptyIter = { [Symbol.iterator]: function* () {} };
  const l = [...emptyIter];
  const m = [];
  m.push(...emptyIter);
  __L(10, "empty-iter => " + JSON.stringify(l) + " " + JSON.stringify(m) + " " + l.length + m.length);
}

summary("arrays_ext");
