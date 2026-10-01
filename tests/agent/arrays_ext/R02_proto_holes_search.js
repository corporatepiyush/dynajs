__EXP = {};
__EXP[0] = ["A 1,3,true,1,3"];
__EXP[1] = ["B 1:true:1"];
__EXP[2] = ["C -1:1:false"];
__EXP[3] = ["D -1:false:-1"];
__EXP[4] = ["E 1,-1,true,3"];
__EXP[5] = ["F 1:true"];
__EXP[6] = ["G 1:-1:1"];
__EXP[7] = ["H 1:true:2"];
const out = typeof console !== "undefined" ? console.log : print;
const P = Array.prototype;

function clean() { delete P[1]; delete P[3]; delete P[2]; }
function withProto(fn) {
  P[1] = "P1"; P[3] = "P3";
  try { return fn(); } finally { clean(); }
}

__L(0, "A " + withProto(() => {
  const a = [1, , 3, , 5];
  return [a.indexOf("P1"), a.indexOf("P3"), a.includes("P1"), a.lastIndexOf("P1"), a.lastIndexOf("P3")].join(",");
}));
__L(1, "B " + withProto(() => {
  P[1] = undefined;
  const a = [1, , 3];
  return a.indexOf(undefined) + ":" + a.includes(undefined) + ":" + a.lastIndexOf(undefined);
}));
__L(2, "C " + withProto(() => {
  const a = [1, 9, 3];
  return a.indexOf("P1") + ":" + a.indexOf(9) + ":" + a.includes("P3");
}));
Object.prototype[2] = "O2";
try {
  const a = [1, , 3];
  __L(3, "D " + a.indexOf("O2") + ":" + a.includes("O2") + ":" + a.lastIndexOf("O2"));
} finally { delete Object.prototype[2]; }
__L(4, "E " + withProto(() => {
  const a = new Array(4);
  return [a.indexOf("P1"), a.indexOf(undefined), a.includes(undefined), a.lastIndexOf("P3")].join(",");
}));
__L(5, "F " + withProto(() => {
  const a = Object.freeze([1, , 3]);
  return a.indexOf("P1") + ":" + a.includes("P1");
}));
__L(6, "G " + (() => {
  P[1] = "P1";
  const a = [1, , 3];
  const first = a.indexOf("P1");
  delete P[1];
  const second = a.indexOf("P1");
  P[1] = "P1";
  const third = a.indexOf("P1");
  delete P[1];
  return first + ":" + second + ":" + third;
})());
__L(7, "H " + (() => {
  let gets = 0;
  Object.defineProperty(P, "1", { get() { gets++; return "GA"; }, configurable: true });
  try {
    const a = [1, , 3];
    const i = a.indexOf("GA");
    const c = a.includes("GA");
    return i + ":" + c + ":" + gets;
  } finally { delete P[1]; }
})());

summary("arrays_ext");
