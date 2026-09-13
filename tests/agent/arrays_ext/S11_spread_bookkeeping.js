// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A sparse len=5 owns=11111 keys=[\"0\",\"1\",\"2\",\"3\",\"4\"]"];
__EXP[1] = ["B holey-tail len=4 owns=1111 keys=[\"0\",\"1\",\"2\",\"3\"]"];
__EXP[2] = ["C elisions len=3 owns=010 keys=[\"1\"]"];
__EXP[3] = ["D tail-sparse len=10001 owns=111111111111111111111111111111111111111111111 lenonly=10001"];
__EXP[4] = ["E keys=[\"0\",\"1\",\"2\",\"z\",\"a\"] len=3"];
__EXP[5] = ["F ctor=Array isArray=true subIsArray=true [1,2]"];
__EXP[6] = ["G len=1 owns=1 keys=[\"0\"] neg=undefined max32=undefined"];
// S11: spread result bookkeeping — length, index own-ness, key order on adversarial shapes
const out = typeof console !== "undefined" ? console.log : print;

function probe(r) {
  const owns = [];
  for (let i = 0; i < r.length; i++) owns.push(Object.prototype.hasOwnProperty.call(r, i) ? 1 : 0);
  return "len=" + r.length + " owns=" + owns.join("") + " keys=" + JSON.stringify(Object.keys(r));
}
{
  const a = [];
  a.length = 5;
  a[2] = "two";
  __L(0, "A sparse " + probe([...a]));
}
{
  const b = [1, , 3, , ];
  __L(1, "B holey-tail " + probe([...b]));
}
{
  const c = [, ...[1], , ];
  __L(2, "C elisions " + probe(c));
}
{
  const d = [7, 8];
  d[10000] = "far";
  __L(3, "D tail-sparse " + probe([...d]).slice(0, 60) + " lenonly=" + [...d].length);
}
{
  // key order: spread result keys are canonical ascending indices first
  const e = Object.assign([...[1, 2, 3]], { z: 1, a: 2 });
  __L(4, "E keys=" + JSON.stringify(Object.keys(e)) + " len=" + e.length);
}
{
  // spread result constructor is plain Array even from subclass sources
  class Sub extends Array {}
  const sub = Sub.from([1, 2]);
  const r = [...sub];
  __L(5, "F ctor=" + r.constructor.name + " isArray=" + Array.isArray(r) +
      " subIsArray=" + Array.isArray(sub) + " " + JSON.stringify(r));
}
{
  // spread of an array with a NEGATIVE-ish and out-of-range string keys — excluded
  const g = [1];
  g["-1"] = "neg";
  g["1e3"] = "thou";
  g["4294967295"] = "max32"; // 2^32-1 is NOT an array index
  const r = [...g];
  __L(6, "G " + probe(r) + " neg=" + r["-1"] + " max32=" + r["4294967295"]);
}

summary("arrays_ext");
