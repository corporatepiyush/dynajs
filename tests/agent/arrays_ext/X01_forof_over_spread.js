// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["copy => 1,2,3 srclen=6 srchead=[1,2,3,10,20,30]"];
__EXP[1] = ["live => 1,2,3,11,12,13,21,22,23,31,32,33,41,42,43,51,52,53,61,62,63 len=24"];
__EXP[2] = ["snapshot => 6 [1,2,3] 4"];
__EXP[3] = ["proto-copy => 0,PX,2"];
__EXP[4] = ["nested => 1:1 1:2 2:1 2:2 twice=[1,2]"];
// X01: for-of over a spread RESULT — copy decouples iteration from the mutating source
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [1, 2, 3];
  const log = [];
  for (const v of [...a]) {
    log.push(v);
    a.push(v * 10); // source grows; the copy does not
    if (log.length > 20) break;
  }
  __L(0, "copy => " + log.join(",") + " srclen=" + a.length + " srchead=" + JSON.stringify(a.slice(0, 6)));
}
{
  // spread INSIDE the loop head vs outside
  const b = [1, 2, 3];
  const log2 = [];
  for (const v of b) { // no copy: growth IS observed
    log2.push(v);
    b.push(v + 10);
    if (log2.length > 20) break;
  }
  __L(1, "live => " + log2.join(",") + " len=" + b.length);
}
{
  // spread result of a MUTATED-during-spread source: snapshot semantics
  const c = [1, 2, 3];
  const snapshot = [...c];
  c.push("late");
  let sum = 0;
  for (const v of snapshot) sum += v;
  __L(2, "snapshot => " + sum + " " + JSON.stringify(snapshot) + " " + c.length);
}
{
  // for-of over [...x] where x is sparse with proto props — proto values in the copy
  Array.prototype[1] = "PX";
  try {
    const d = [0, , 2];
    const got = [];
    for (const v of [...d]) got.push(v === undefined ? "U" : v);
    __L(3, "proto-copy => " + got.join(","));
  } finally { delete Array.prototype[1]; }
}
{
  // nested: for-of over spread result of a for-of over spread result
  const e = [1, 2];
  const twice = [...[...e]];
  const log3 = [];
  for (const v of twice) for (const w of [...e]) log3.push(v + ":" + w);
  __L(4, "nested => " + log3.join(" ") + " twice=" + JSON.stringify(twice));
}

summary("arrays_ext");
