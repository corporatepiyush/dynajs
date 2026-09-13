// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["sparse => len=100000 r7=seven r99999=tail r6=undefined r0=undefined own6=true"];
__EXP[1] = ["proto => len=10 r3=P3 own3=true r5=five"];
__EXP[2] = ["rev => last head len=100000"];
__EXP[3] = ["slice => [\"last\",null] [null,\"head\"]"];
__EXP[4] = ["mixed => len=54 [0,2]=1,undefined 27=mid end=[3,4]"];
__EXP[5] = ["double => len=12 [\"two\",\"two\"] holes=0"];
// S04: 1e5-hole sparse array through spread — bulk-append fast path over dictionary elements
const out = typeof console !== "undefined" ? console.log : print;
const N = 100000;

{
  const a = [];
  a.length = N;
  a[7] = "seven";
  a[99999] = "tail";
  const r = [...a];
  __L(0, "sparse => len=" + r.length + " r7=" + r[7] + " r99999=" + r[99999] +
      " r6=" + r[6] + " r0=" + r[0] + " own6=" + Object.prototype.hasOwnProperty.call(r, 6));
}
{
  // sparse with proto indexed props: holes read the proto through Get during iteration
  Array.prototype[3] = "P3";
  try {
    const b = [];
    b.length = 10;
    b[5] = "five";
    const r2 = [...b];
    __L(1, "proto => len=" + r2.length + " r3=" + r2[3] + " own3=" +
        Object.prototype.hasOwnProperty.call(r2, 3) + " r5=" + r2[5]);
  } finally { delete Array.prototype[3]; }
}
{
  // spread of a sparse array, result then reversed/sliced (fast-path result re-enters mutators)
  const c = [];
  c.length = N;
  c[0] = "head";
  c[N - 1] = "last";
  const r3 = [...c];
  r3.reverse();
  __L(2, "rev => " + r3[0] + " " + r3[r3.length - 1] + " len=" + r3.length);
  __L(3, "slice => " + JSON.stringify(r3.slice(0, 2)) + " " + JSON.stringify(r3.slice(-2)));
}
{
  // spread WITHIN a larger literal: dense, sparse, dense
  const d = [];
  d.length = 50;
  d[25] = "mid";
  const r4 = [1, 2, ...d, 3, 4];
  __L(4, "mixed => len=" + r4.length + " [0,2]=" + r4[0] + "," + r4[2] + " 27=" + r4[27] + " end=" + JSON.stringify(r4.slice(-2)));
}
{
  // double spread of the same sparse array
  const e = [];
  e.length = 6;
  e[2] = "two";
  const r5 = [...e, ...e];
  __L(5, "double => len=" + r5.length + " " + JSON.stringify([r5[2], r5[8]]) +
      " holes=" + [0, 1, 3, 4, 5, 6, 7, 9, 10, 11].filter((i) => !Object.prototype.hasOwnProperty.call(r5, i)).length);
}

summary("arrays_ext");
