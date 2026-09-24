// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["e0  [\"a\",\"b\"]"];
__EXP[1] = ["e1  [null,\"a\",\"b\"]"];
__EXP[2] = ["e2  [\"a\",\"b\",null]"];
__EXP[3] = ["e3  [null,\"a\",\"b\",null]"];
__EXP[4] = ["e4  [\"a\",\"b\",null,\"a\",\"b\"]"];
__EXP[5] = ["e5  [null,null,\"a\",\"b\"]"];
__EXP[6] = ["e6  [\"a\",\"b\",null,null]"];
__EXP[7] = ["e7  [null,null,\"a\",\"b\",null,null]"];
__EXP[8] = ["e8  [1,\"a\",\"b\",null,2]"];
__EXP[9] = ["e9  [null,1,\"a\",\"b\",2,null]"];
__EXP[10] = ["e10 [1,2]"];
__EXP[11] = ["e11 [1,2]"];
__EXP[12] = ["e12 [1,2]"];
__EXP[13] = ["e13 [null,null,1]"];
__EXP[14] = ["e14 []"];
__EXP[15] = ["e15 [null,null,null]"];
__EXP[16] = ["e16 [null]"];
__EXP[17] = ["e17 [null,null,\"a\",\"b\"]"];
__EXP[18] = ["e18 [null,\"a\",\"b\",null,\"a\",\"b\",null]"];
__EXP[19] = ["e19 [0,\"a\",\"b\",null,null,9]"];
__EXP[20] = ["e1b  len=4 holes@0,3,4=3 r1=a r2=b"];
__EXP[21] = ["e4b  len=5 hole2=true [\"a\",\"b\",\"a\",\"b\"]"];
__EXP[22] = ["e13b len=2 hole0=true hole2=true r3[1]=undefined"];
__EXP[23] = ["nest len=5 holes=0 [\"x\",\"y\"]"];
// S08: elisions around spreads — elision around spread (regression class: memory corruption via count-over-write).
// EVERY variant must produce length-correct, position-correct arrays.
const out = typeof console !== "undefined" ? console.log : print;
const src = () => ["a", "b"];

__L(0, "e0  " + JSON.stringify([...src()]));
__L(1, "e1  " + JSON.stringify([, ...src()]));
__L(2, "e2  " + JSON.stringify([...src(), ,]));
__L(3, "e3  " + JSON.stringify([, ...src(), ,]));
__L(4, "e4  " + JSON.stringify([...src(), , ...src()]));
__L(5, "e5  " + JSON.stringify([, , ...src()]));
__L(6, "e6  " + JSON.stringify([...src(), , ,]));
__L(7, "e7  " + JSON.stringify([, , ...src(), , ,]));
__L(8, "e8  " + JSON.stringify([1, ...src(), , 2]));
__L(9, "e9  " + JSON.stringify([, 1, ...src(), 2, ,]));
__L(10, "e10 " + JSON.stringify([...[], 1, 2]));
__L(11, "e11 " + JSON.stringify([1, ...[], 2]));
__L(12, "e12 " + JSON.stringify([1, 2, ...[]]));
__L(13, "e13 " + JSON.stringify([, ...[], , 1]));
__L(14, "e14 " + JSON.stringify([...[], ...[], ...[]]));
__L(15, "e15 " + JSON.stringify([, , ,]));
__L(16, "e16 " + JSON.stringify([,]));
__L(17, "e17 " + JSON.stringify([...[], , , ...src()]));
__L(18, "e18 " + JSON.stringify([, ...src(), ...[], , ...src(), ,]));
__L(19, "e19 " + JSON.stringify([0, ...src(), , ...[], , 9]));

// length + hole bookkeeping, not just JSON (JSON hides hole vs undefined distinction... for
// trailing elisions JSON.stringify drops them, so verify hasOwnProperty directly)
{
  const r = [, ...src(), ,];
  const holes = [0, 3, 4].filter((i) => !Object.prototype.hasOwnProperty.call(r, i)).length;
  __L(20, "e1b  len=" + r.length + " holes@0,3,4=" + holes + " r1=" + r[1] + " r2=" + r[2]);
}
{
  const r2 = [...src(), , ...src()];
  __L(21, "e4b  len=" + r2.length + " hole2=" + !Object.prototype.hasOwnProperty.call(r2, 2) +
      " " + JSON.stringify([r2[0], r2[1], r2[3], r2[4]]));
}
{
  // elision + EMPTY spread + elision — expand_fast_array size-0 early-return territory
  const r3 = [ , ...[], , ];
  __L(22, "e13b len=" + r3.length + " hole0=" + !Object.prototype.hasOwnProperty.call(r3, 0) +
      " hole2=" + !Object.prototype.hasOwnProperty.call(r3, 2) + " r3[1]=" + r3[1]);
}
{
  // nested: spread-of-literal-containing-elisions
  const r4 = [...[, , "x", , "y"]];
  __L(23, "nest len=" + r4.length + " holes=" + [0, 1, 3].filter((i) => !Object.prototype.hasOwnProperty.call(r4, i)).length +
      " " + JSON.stringify([r4[2], r4[4]]));
}

summary("arrays_ext");
