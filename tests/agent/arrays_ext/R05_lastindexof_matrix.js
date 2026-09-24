// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A 4 0 0 4 3 1 2 -1 -1"];
__EXP[1] = ["B 1 1 -1 -1 -1 -1"];
__EXP[2] = ["C 3 -1 2 1 0 -1 0 -1"];
__EXP[3] = ["D 99998 99998 -1 -1"];
__EXP[4] = ["E 1 1 2"];
__EXP[5] = ["F -1 -1 0 -1"];
// R05: lastIndexOf fromIndex exhaustive matrix on holey arrays x proto props
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

function L(arr, needle, from) {
  return from === undefined ? arr.lastIndexOf(needle) : arr.lastIndexOf(needle, from);
}
{
  const a = [1, 2, 3, 2, 1];
  __L(0, "A " + L(a, 1) + " " + L(a, 1, 1) + " " + L(a, 1, 0) + " " + L(a, 1, -1) + " " +
      L(a, 2, -2) + " " + L(a, 2, -3) + " " + L(a, 3, 100) + " " + L(a, 9) + " " + L(a, 9, -99));
}
{
  // sparse + proto: lastIndexOf walks DOWN through HasProperty — proto values at holes count
  P[1] = "P1";
  try {
    const b = [1, , 3, , 5];
    __L(1, "B " + L(b, "P1") + " " + L(b, "P1", 2) + " " + L(b, "P1", 0) + " " +
        L(b, "P3", 3) + " " + L(b, undefined) + " " + L(b, undefined, 3));
  } finally { delete P[1]; }
}
{
  // fromIndex coercions: undefined->len, NaN->len, "2"->2, 1.9->1, -0.5->len-... fractional neg
  const c = [0, 1, 2, 3];
  __L(2, "C " + L(c, 3, undefined) + " " + L(c, 3, NaN) + " " + L(c, 2, "2") + " " +
      L(c, 1, 1.9) + " " + L(c, 0, null) + " " + L(c, 3, true) + " " + L(c, 0, "") + " " +
      L(c, 3, -0.5));
}
{
  // sparse 1e5: lastIndexOf near the tail must not scan the empty head
  const d = [];
  d.length = 100000;
  d[99998] = "x";
  __L(3, "D " + L(d, "x") + " " + L(d, "x", 99999) + " " + L(d, "x", 99997) + " " + L(d, undefined, 10));
}
{
  // frozen + sealed holey lastIndexOf
  const e = Object.freeze([1, , 3]);
  P[1] = "FZ";
  try {
    __L(4, "E " + L(e, "FZ") + " " + L(Object.seal([, 9]), 9) + " " + L(e, 3, 2));
  } finally { delete P[1]; }
}
{
  // lastIndexOf on length-0 and fromIndex 0
  __L(5, "F " + [].lastIndexOf(undefined) + " " + [].lastIndexOf(undefined, 0) + " " +
      [7].lastIndexOf(7, 0) + " " + [7].lastIndexOf(8, 0));
}

summary("arrays_ext");
