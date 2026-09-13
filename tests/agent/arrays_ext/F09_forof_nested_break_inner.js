// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["basic => o1 i10 i20 after1 o2 i10 i20 after2 o3 i10 i20 after3"];
__EXP[1] = ["finally => f10 | f10 |"];
__EXP[2] = ["triple => 4"];
__EXP[3] = ["matrix => A1x . A2x . B1x . B2x ."];
__EXP[4] = ["labels => 1:10"];
__EXP[5] = ["pairs => 1,2,3,4"];
// F09: break in nested for-of closes the inner loop only; try/finally interplay
const out = typeof console !== "undefined" ? console.log : print;

{
  const log = [];
  for (const v of [1, 2, 3]) {
    log.push("o" + v);
    for (const w of [10, 20, 30]) {
      log.push("i" + w);
      if (w === 20) break;
    }
    log.push("after" + v);
  }
  __L(0, "basic => " + log.join(" "));
}
{
  const log2 = [];
  for (const v of [1, 2]) {
    for (const w of [10, 20]) {
      try { if (w === 10) break; log2.push("nf" + w); }
      finally { log2.push("f" + w); }
    }
    log2.push("|");
  }
  __L(1, "finally => " + log2.join(" "));
}
{
  const log3 = [];
  for (const a of [1])
    for (const b of [2, 3]) {
      for (const c of [4, 5]) { log3.push(c); if (c === 4) break; }
      if (b === 2) break;
    }
  __L(2, "triple => " + log3.join(","));
}
// break inner of three arrays; outer pair must both complete
{
  const log4 = [];
  for (const a of ["A", "B"])
    for (const b of [1, 2]) {
      for (const c of ["x", "y"]) { log4.push(a + b + c); break; }
      log4.push(".");
    }
  __L(3, "matrix => " + log4.join(" "));
}
// labeled continue/break across for-of levels
{
  const log5 = [];
  outer: for (const v of [1, 2, 3]) {
    for (const w of [10, 20, 30]) {
      if (w === 20) continue outer;
      if (v === 2 && w === 10) break outer;
      log5.push(v + ":" + w);
    }
  }
  __L(4, "labels => " + log5.join(" "));
}
// for-of over an array whose elements are arrays; break inside the inner VALUE loop
{
  const log6 = [];
  for (const pair of [[1, 2], [3, 4]]) {
    for (const x of pair) { log6.push(x); if (x % 2 === 0) break; }
  }
  __L(5, "pairs => " + log6.join(","));
}

summary("arrays_ext");
