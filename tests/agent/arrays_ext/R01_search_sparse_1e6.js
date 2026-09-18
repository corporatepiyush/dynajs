// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["idx => 10 500000 999999 -1"];
__EXP[1] = ["idx-undef => -1 -1 (expect -1 -1)"];
__EXP[2] = ["inc-undef => true true false"];
__EXP[3] = ["nan => idx=-1 inc=true last=-1"];
__EXP[4] = ["zero => idx0=50 idxm0=50 inc0=true"];
__EXP[5] = ["obj => 250 -1 true"];
__EXP[6] = ["from => 500000 -1 -1 500000 999999 true"];
__EXP[7] = ["coerce => 1 1 2 0 1 -1"];
// R01: indexOf/includes/lastIndexOf on a 1e6-sparse array — occupied-entry scan semantics
const out = typeof console !== "undefined" ? console.log : print;
const N = 1000000;

function mkSparse() {
  const a = [];
  a.length = N;
  a[10] = "ten";
  a[500000] = "mid";
  a[N - 1] = "last";
  return a;
}
{
  const a = mkSparse();
  __L(0, "idx => " + a.indexOf("ten") + " " + a.indexOf("mid") + " " + a.indexOf("last") + " " + a.indexOf("nope"));
}
{
  // indexOf SKIPS holes (HasProperty gate): undefined NOT found despite Get(hole)=undefined
  const a = mkSparse();
  __L(1, "idx-undef => " + a.indexOf(undefined) + " " + a.indexOf() + " (expect -1 -1)");
}
{
  // includes reads EVERY slot: first hole at 0 -> undefined found at 0
  const a = mkSparse();
  __L(2, "inc-undef => " + a.includes(undefined) + " " + a.includes("mid") + " " + a.includes("nope"));
}
{
  // NaN: includes uses SameValueZero (finds), indexOf strict (misses)
  const b = [];
  b.length = 1000;
  b[999] = NaN;
  __L(3, "nan => idx=" + b.indexOf(NaN) + " inc=" + b.includes(NaN) + " last=" + b.lastIndexOf(NaN));
}
{
  // -0 vs +0: both strict-equal and SameValueZero collapse -0/0 — find index of the zero
  const c = [];
  c.length = 100;
  c[50] = 0;
  c[51] = -0;
  const mz = -0;
  __L(4, "zero => idx0=" + c.indexOf(0) + " idxm0=" + c.indexOf(mz) + " inc0=" + c.includes(mz));
}
{
  // object needle: identity only, two equal-looking objects are different
  const d = [];
  d.length = 500;
  const o1 = { id: 1 };
  d[250] = o1;
  d[251] = { id: 1 };
  __L(5, "obj => " + d.indexOf(o1) + " " + d.indexOf({ id: 1 }) + " " + d.includes(o1));
}
{
  // fromIndex windows into the sparse range
  const a = mkSparse();
  __L(6, "from => " + a.indexOf("mid", 499999) + " " + a.indexOf("mid", 500001) +
      " " + a.lastIndexOf("mid", 499999) + " " + a.lastIndexOf("mid") +
      " " + a.indexOf("last", -1) + " " + a.includes("last", N - 2));
}
{
  // undefined fromIndex / NaN fromIndex / string fromIndex coercions
  const e = ["a", "b", "c"];
  __L(7, "coerce => " + e.indexOf("b", undefined) + " " + e.indexOf("b", "1") +
      " " + e.indexOf("c", NaN) + " " + e.indexOf("a", null) + " " + e.indexOf("b", true) +
      " " + e.lastIndexOf("c", 1.9));
}

summary("arrays_ext");
