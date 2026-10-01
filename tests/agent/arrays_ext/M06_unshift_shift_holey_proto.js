__EXP = {};
__EXP[0] = ["A now=[0,1,\"P1\",3] own1=true own2=true a1=1 a2=P1 join=0,1,P1,3"];
__EXP[1] = ["B now=[\"P1\",3] len=2 own0=true own1=true b0=P1 b1=3"];
__EXP[2] = ["C now=[null,3,4] len=3 owns=falsetruetrue"];
__EXP[3] = ["D now=[0,1,\"O1\",3] own1=true d1=1 d2=O1"];
__EXP[4] = ["E now=[\"x\",\"y\",1,\"P1\",3,null,5] owns=111101 join=x,y,1,P1,3,,5"];
__EXP[5] = ["F frozen-shift => TypeError"];
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

{
  P[1] = "P1";
  try {
    const a = [1, , 3];
    a.unshift(0);
    __L(0, "A now=" + JSON.stringify(a) + " own1=" + Object.prototype.hasOwnProperty.call(a, 1) +
        " own2=" + Object.prototype.hasOwnProperty.call(a, 2) +
        " a1=" + a[1] + " a2=" + a[2] + " join=" + a.join(","));
  } finally { delete P[1]; }
}
{
  P[1] = "P1";
  try {
    const b = [1, , 3];
    b.shift();
    __L(1, "B now=" + JSON.stringify(b) + " len=" + b.length +
        " own0=" + Object.prototype.hasOwnProperty.call(b, 0) +
        " own1=" + Object.prototype.hasOwnProperty.call(b, 1) + " b0=" + b[0] + " b1=" + b[1]);
  } finally { delete P[1]; }
}
{
  const c = [1, , 3, 4];
  c.shift();
  __L(2, "C now=" + JSON.stringify(c) + " len=" + c.length +
      " owns=" + [0, 1, 2].map((i) => Object.prototype.hasOwnProperty.call(c, i)).join(""));
}
{
  Object.prototype[1] = "O1";
  try {
    const d = [1, , 3];
    d.unshift(0);
    __L(3, "D now=" + JSON.stringify(d) + " own1=" + Object.prototype.hasOwnProperty.call(d, 1) +
        " d1=" + d[1] + " d2=" + d[2]);
  } finally { delete Object.prototype[1]; }
}
{
  P[1] = "P1";
  try {
    const e = [1, , 3, , 5];
    e.unshift("x", "y");
    __L(4, "E now=" + JSON.stringify(e) + " owns=" +
        [1, 2, 3, 4, 5, 6].map((i) => Object.prototype.hasOwnProperty.call(e, i) ? 1 : 0).join("") +
        " join=" + e.join(","));
  } finally { delete P[1]; }
}
{
  const f = Object.freeze([, 2]);
  let r;
  try { f.shift(); r = "NO-THROW len=" + f.length; } catch (e) { r = e.name; }
  __L(5, "F frozen-shift => " + r);
}

summary("arrays_ext");
