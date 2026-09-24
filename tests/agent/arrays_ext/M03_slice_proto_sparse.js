// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A slice-all=[1,\"P1\",3,4] slice-mid=[1,\"P1\",3] own1=true"];
__EXP[1] = ["B slice=[1,null,3] own1=false s1=undefined inc=true"];
__EXP[2] = ["C neg=[\"eight\",\"nine\"] [\"nine\"] [null,null,null,null,null,null,null,null,\"eight\",\"nine\"] [] len=10"];
__EXP[3] = ["D huge=[null,\"last\"] slen=2 whole=1000000 first=2"];
__EXP[4] = ["E named=[1,2] tag=undefined len=2"];
__EXP[5] = ["F objproto=[\"O0\",1] own0=true"];
__EXP[6] = ["G fresh=[\"v1\"] [\"v2\"]"];
// M03: slice on holey/slow arrays — proto indexed props are COPIED into the result
// (HasProperty -> Get chain reads), negative ranges, huge sparse bounds
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

{
  P[1] = "P1";
  try {
    const a = [1, , 3, 4];
    __L(0, "A slice-all=" + JSON.stringify(a.slice()) +
        " slice-mid=" + JSON.stringify(a.slice(0, 3)) +
        " own1=" + Object.prototype.hasOwnProperty.call(a.slice(), 1));
  } finally { delete P[1]; }
}
{
  // slice WITHOUT proto keeps holes as holes
  const a = [1, , 3];
  const s = a.slice();
  __L(1, "B slice=" + JSON.stringify(s) + " own1=" + Object.prototype.hasOwnProperty.call(s, 1) +
      " s1=" + s[1] + " inc=" + s.includes(undefined));
}
{
  // negative ranges on sparse
  const c = [];
  c.length = 10;
  c[8] = "eight";
  c[9] = "nine";
  __L(2, "C neg=" + JSON.stringify(c.slice(-2)) + " " + JSON.stringify(c.slice(-1)) +
      " " + JSON.stringify(c.slice(-100)) + " " + JSON.stringify(c.slice(5, -8)) +
      " len=" + c.slice().length);
}
{
  // huge sparse: slice clamps, does not materialize 1e6 elements observably
  const d = [];
  d.length = 1000000;
  d[999999] = "last";
  const s2 = d.slice(999998);
  __L(3, "D huge=" + JSON.stringify(s2) + " slen=" + s2.length +
      " whole=" + d.slice().length + " first=" + d.slice(0, 2).length);
}
{
  // slice on a slow array with named props: named props NOT copied
  const e = [1, 2, 3];
  e.tag = "T";
  e[100000] = "far";
  const s3 = e.slice(0, 2);
  __L(4, "E named=" + JSON.stringify(s3) + " tag=" + s3.tag + " len=" + s3.length);
}
{
  // slice where proto appears on Object.prototype
  Object.prototype[0] = "O0";
  try {
    const f = [, 1];
    __L(5, "F objproto=" + JSON.stringify(f.slice()) + " own0=" +
        Object.prototype.hasOwnProperty.call(f.slice(), 0));
  } finally { delete Object.prototype[0]; }
}
{
  // proto value mutated BETWEEN two slices — second slice sees the fresh value
  P[2] = "v1";
  try {
    const g = [0, 1, , 3];
    const s1 = g.slice(2, 3);
    P[2] = "v2";
    const s2 = g.slice(2, 3);
    __L(6, "G fresh=" + JSON.stringify(s1) + " " + JSON.stringify(s2));
  } finally { delete P[2]; }
}

summary("arrays_ext");
