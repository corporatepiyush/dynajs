// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["neg => 0 3 2 -1 1 3 false true true"];
__EXP[1] = ["big => -1 false 0 -1 false -1"];
__EXP[2] = ["2g31 => idx=2147483647 inc=true last=2147483647 lastNeg=2147483647 miss=2147483647"];
__EXP[3] = ["frozen => 1 2 true -1"];
__EXP[4] = ["sealed-holey => 1 true 2"];
__EXP[5] = ["round => 100000,1,clean | -1,5,2 | 99999,-1"];
__EXP[6] = ["named => 1 1 true hole=true -1 after-delete-proto"];
// R03: search fromIndex edges — negative beyond length, 2^31-1 index, frozen/sealed, fast<->slow round trips
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [1, 2, 3, 2, 1];
  __L(0, "neg => " + a.indexOf(1, -100) + " " + a.indexOf(2, -2) + " " + a.indexOf(3, -3) +
      " " + a.lastIndexOf(1, -100) + " " + a.lastIndexOf(2, -3) + " " + a.lastIndexOf(2, -2) +
      " " + a.includes(3, -2) + " " + a.includes(3, -3) + " " + a.includes(1, -1));
}
{
  // fromIndex > length
  __L(1, "big => " + [1, 2].indexOf(1, 5) + " " + [1, 2].includes(1, 5) + " " + [1, 2].lastIndexOf(1, 5) +
      " " + [].indexOf(1) + " " + [].includes(1) + " " + [].lastIndexOf(1));
}
{
  // single element at index 2^31-1 (dictionary element), length 2^31
  const a = [];
  a[2147483647] = "hi";
  a.length = 2147483648;
  __L(2, "2g31 => idx=" + a.indexOf("hi", 2147483647) + " inc=" + a.includes("hi", 2147483647) +
      " last=" + a.lastIndexOf("hi") + " lastNeg=" + a.lastIndexOf("hi", -1) +
      " miss=" + a.indexOf("hi", 2147483646));
}
{
  // frozen / sealed dense
  const f = Object.freeze([5, 6, 7]);
  const s = Object.seal([5, , 7]);
  __L(3, "frozen => " + f.indexOf(6) + " " + f.lastIndexOf(7) + " " + f.includes(6) + " " + f.indexOf(9));
  Array.prototype[1] = "PX";
  try {
    __L(4, "sealed-holey => " + s.indexOf("PX") + " " + s.includes("PX") + " " + s.indexOf(7));
  } finally { delete Array.prototype[1]; }
}
{
  // fast -> slow round trip: named props then huge index then shrink again
  const a = [1, 2, 3];
  a.named = "n";
  a[100000] = "far";
  const r1 = [a.indexOf("far"), a.indexOf(2), a.includes("n" ) ? "propleak" : "clean"];
  a.length = 5;
  const r2 = [a.indexOf("far"), a.length, a.indexOf(3)];
  a.length = 100000;
  a[99999] = "back";
  const r3 = [a.indexOf("back"), a.indexOf("far")];
  __L(5, "round => " + r1.join(",") + " | " + r2.join(",") + " | " + r3.join(","));
}
{
  // slow (dictionary) array with named prop only — search fast path must not read named props
  const b = [1, 2, 3];
  b.val = 2;
  __L(6, "named => " + b.indexOf(2) + " " + b.lastIndexOf(2) + " " + b.includes(2) +
      " hole=" + (delete b[1]) + " " + b.indexOf(2) + " after-delete-proto");
}

summary("arrays_ext");
