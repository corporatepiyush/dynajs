// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A concat=[1,\"P1\",3,4,5] own1=true len=5"];
__EXP[1] = ["B ta=[1,{\"0\":7,\"1\":8},9] isArray=true"];
__EXP[2] = ["C mix=[0,{\"0\":1.5,\"1\":2.5},null,null,null,\"s3\"] len=6 holes=2"];
__EXP[3] = ["D objproto=[0,1,\"O2\",null,\"O2\"] own2=false"];
__EXP[4] = ["E slowrecv=[0,null,null,null] len=100003 tag=undefined far-present=true isArray=true"];
__EXP[5] = ["F like=[0,{\"0\":\"x\",\"1\":\"y\",\"length\":2}] len=2"];
__EXP[6] = ["G spreadable=[0,\"x\",\"y\"] len=3"];
__EXP[7] = ["H self=[1,2,1,2] len=4 orig=2"];
// M04: concat of holey + dense + typed + proto — Get-per-index semantics (proto values copied)
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

{
  P[1] = "P1";
  try {
    const holey = [1, , 3];
    const dense = [4, 5];
    const r = holey.concat(dense);
    __L(0, "A concat=" + JSON.stringify(r) + " own1=" + Object.prototype.hasOwnProperty.call(r, 1) +
        " len=" + r.length);
  } finally { delete P[1]; }
}
{
  // concat a typed array: spread per index, result is a plain Array
  const ta = new Uint8Array([7, 8]);
  const r2 = [1].concat(ta, [9]);
  __L(1, "B ta=" + JSON.stringify(r2) + " isArray=" + Array.isArray(r2));
}
{
  // concat dense + holey + typed + sparse-tail
  const sparse = [];
  sparse.length = 4;
  sparse[3] = "s3";
  const r3 = [0].concat(new Float64Array([1.5, 2.5]), sparse);
  __L(2, "C mix=" + JSON.stringify(r3) + " len=" + r3.length +
      " holes=" + [1, 2, 4, 5].filter((i) => !Object.prototype.hasOwnProperty.call(r3, i)).length);
}
{
  // concat with proto on Object.prototype at a hole
  Object.prototype[2] = "O2";
  try {
    const r4 = [0, 1].concat([, , ,]);
    __L(3, "D objproto=" + JSON.stringify(r4) + " own2=" + Object.prototype.hasOwnProperty.call(r4, 2));
  } finally { delete Object.prototype[2]; }
}
{
  // concat onto a slow/dictionary receiver
  const recv = [0];
  recv.tag = "T";
  recv[100000] = "far";
  const r5 = recv.concat([1, 2]);
  __L(4, "E slowrecv=" + JSON.stringify(r5.slice(0, 4)) + " len=" + r5.length +
      " tag=" + r5.tag + " far-present=" + (r5[100000] === "far") + " isArray=" + Array.isArray(r5));
}
{
  // concat arguments-like object (spreadable? NO — only @@isConcatSpreadable or Array/TA)
  const like = { length: 2, 0: "x", 1: "y" };
  const r6 = [0].concat(like);
  __L(5, "F like=" + JSON.stringify(r6) + " len=" + r6.length);
  like[Symbol.isConcatSpreadable] = true;
  const r7 = [0].concat(like);
  __L(6, "G spreadable=" + JSON.stringify(r7) + " len=" + r7.length);
}
{
  // concat self — snapshot before append
  const a = [1, 2];
  const r8 = a.concat(a);
  __L(7, "H self=" + JSON.stringify(r8) + " len=" + r8.length + " orig=" + a.length);
}

summary("arrays_ext");
