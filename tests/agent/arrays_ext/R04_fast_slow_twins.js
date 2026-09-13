// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["twins => PARITY\nfast=1,7,2,true,-1,true,4,5,4\nslow=1,7,2,true,-1,true,4,5,4"];
__EXP[1] = ["twins-from => PARITY 7,1,false,5"];
__EXP[2] = ["coerce => PARITY -1,-1,-1,-1,-1,3"];
__EXP[3] = ["named-idx => 0 1 -1 len=4 inc=true"];
__EXP[4] = ["proto-undef => true 1"];
// R04: fast vs slow twins — identical content, different internal representation, identical search results
const out = typeof console !== "undefined" ? console.log : print;

function slowify(a) { // force dictionary elements in any engine
  a[4294967294] = "sentinel-sparse";
  a.length = 10;
  return a;
}
function twins() {
  const fast = [3, "x", undefined, NaN, 0, -0, {}, "x"];
  const slow = slowify([3, "x", undefined, NaN, 0, -0, {}, "x"]);
  return [fast, slow];
}
function probe(a) {
  return [
    a.indexOf("x"), a.lastIndexOf("x"),
    a.indexOf(undefined), a.includes(undefined),
    a.indexOf(NaN), a.includes(NaN),
    a.indexOf(0), a.lastIndexOf(0),
    a.indexOf(-0),
  ].join(",");
}
{
  const [f, s] = twins();
  const pf = probe(f);
  const ps = probe(s);
  __L(0, "twins => " + (pf === ps ? "PARITY" : "DIVERGE") + "\nfast=" + pf + "\nslow=" + ps);
}
{
  // twins with fromIndex variants
  const [f, s] = twins();
  const pr = (a) => [a.indexOf("x", 2), a.lastIndexOf("x", 6), a.includes(NaN, 4), a.indexOf(0, 5)].join(",");
  __L(1, "twins-from => " + (pr(f) === pr(s) ? "PARITY " : "DIVERGE ") + pr(f));
}
{
  // needle coercions: "2" vs 2, true vs 1, null vs 0 — strict equality, no coercion
  const fast = [2, 1, 0, ""];
  const slow = slowify([2, 1, 0, ""]);
  const pr = (a) => [a.indexOf("2"), a.indexOf("1"), a.indexOf(true), a.indexOf(null), a.indexOf(false), a.indexOf("")].join(",");
  __L(2, "coerce => " + (pr(fast) === pr(slow) ? "PARITY " : "DIVERGE ") + pr(fast));
}
{
  // search on a slow array whose named props LOOK like indices ("0", "1" as strings)
  const a = [9, 9];
  a.length = 4;
  delete a[0]; delete a[1];
  a["0"] = "str0";
  a["1"] = "str1";
  __L(3, "named-idx => " + a.indexOf("str0") + " " + a.indexOf("str1") + " " + a.indexOf(9) +
      " len=" + a.length + " inc=" + a.includes("str1"));
}
{
  // includes on huge holey with proto undefined at holes (spec: Get reads proto -> undefined matches)
  Array.prototype[1] = undefined;
  try {
    const a = [0, , 2];
    __L(4, "proto-undef => " + a.includes(undefined) + " " + a.indexOf(undefined));
  } finally { delete Array.prototype[1]; }
}

summary("arrays_ext");
