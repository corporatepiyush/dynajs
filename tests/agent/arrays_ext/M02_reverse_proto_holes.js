__EXP = {};
__EXP[0] = ["A rev=[4,3,\"P1\",1] own1=true own3=true"];
__EXP[1] = ["B rev=[5,\"P3\",3,\"P1\",1] own1=true own3=true a2=3"];
__EXP[2] = ["C rev=[4,3,\"O1\",1] own1=true"];
__EXP[3] = ["D rev=[4,\"D2\",2,1] own0=true own2=true a0=4"];
__EXP[4] = ["E rev=[null,3,2,1] own0=false own3=true"];
__EXP[5] = ["F rev-len=100001 head=[\"far\",null,null] far@99999=2 tag=T"];
__EXP[6] = ["G frozen-rev => TypeError"];
__EXP[7] = ["H [] [7]"];
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

function run(arr, protoSetup, after) {
  P[1] = "P1"; P[3] = "P3";
  if (protoSetup) protoSetup();
  try {
    arr.reverse();
    return "rev=" + JSON.stringify(arr) + " own1=" + Object.prototype.hasOwnProperty.call(arr, 1) +
      " own3=" + Object.prototype.hasOwnProperty.call(arr, 3) + (after ? " " + after() : "");
  } finally { delete P[1]; delete P[3]; }
}

__L(0, "A " + run([1, , 3, 4]));
{
  const a = [1, , 3, , 5];
  P[1] = "P1"; P[3] = "P3";
  try {
    a.reverse();
    __L(1, "B rev=" + JSON.stringify(a) + " own1=" + Object.prototype.hasOwnProperty.call(a, 1) +
        " own3=" + Object.prototype.hasOwnProperty.call(a, 3) + " a2=" + a[2]);
  } finally { delete P[1]; delete P[3]; }
}
{
  Object.prototype[1] = "O1";
  try {
    const a = [1, , 3, 4];
    a.reverse();
    __L(2, "C rev=" + JSON.stringify(a) + " own1=" + Object.prototype.hasOwnProperty.call(a, 1));
  } finally { delete Object.prototype[1]; }
}
{
  P[2] = "D2";
  try {
    const a = [1, 2, 3, 4];
    delete a[2];
    a.reverse();
    __L(3, "D rev=" + JSON.stringify(a) + " own0=" + Object.prototype.hasOwnProperty.call(a, 0) +
        " own2=" + Object.prototype.hasOwnProperty.call(a, 2) + " a0=" + a[0]);
  } finally { delete P[2]; }
}
{
  const a = [1, 2, 3, 4];
  delete a[3];
  a.reverse();
  __L(4, "E rev=" + JSON.stringify(a) + " own0=" + Object.prototype.hasOwnProperty.call(a, 0) +
      " own3=" + Object.prototype.hasOwnProperty.call(a, 3));
}
{
  const a = [1, 2, 3];
  a.tag = "T";
  a[100000] = "far";
  a.reverse();
  __L(5, "F rev-len=" + a.length + " head=" + JSON.stringify(a.slice(0, 3)) +
      " far@99999=" + a[99999] + " tag=" + a.tag);
}
{
  let r;
  try { Object.freeze([1, 2, 3]).reverse(); r = "NO-THROW"; } catch (e) { r = e.name; }
  __L(6, "G frozen-rev => " + r);
}
__L(7, "H " + JSON.stringify([].reverse()) + " " + JSON.stringify([7].reverse()));

summary("arrays_ext");
