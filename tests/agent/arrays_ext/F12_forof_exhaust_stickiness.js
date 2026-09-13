// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["shrink-after-done => n=10 true true true"];
__EXP[1] = ["regrow-undone => undefined:true"];
__EXP[2] = ["regrow-gets-new => undefined:true"];
__EXP[3] = ["boundary => undefined:true undefined:true"];
__EXP[4] = ["done-increments => undefined:true"];
__EXP[5] = ["slow-mid => 1,2,3 slow"];
// F12: exhaustion transition — done stays done after shrink; NOT sticky after huge regrow
// spec: index increments even on the done path, so regrowing far enough un-dones it
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
  const it = a[Symbol.iterator]();
  let n = 0, v;
  while (!(v = it.next()).done) n++;
  const d1 = it.next().done;
  const d2 = it.next().done;
  a.length = 5; // shrink
  const d3 = it.next().done;
  __L(0, "shrink-after-done => n=" + n + " " + d1 + " " + d2 + " " + d3);
}
{
  const a2 = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
  const it2 = a2[Symbol.iterator]();
  for (let i = 0; i < 12; i++) it2.next(); // 10 values + 2 done (index now 12)
  a2.length = 1000000; // huge regrow — cursor 12 < 1e6
  const r = it2.next();
  __L(1, "regrow-undone => " + r.value + ":" + r.done);
  a2[12] = "twelve";
  const r2 = it2.next();
  __L(2, "regrow-gets-new => " + r2.value + ":" + r2.done);
}
{
  // exact boundary: regrow to EXACTLY the current index
  const a3 = [0, 1, 2];
  const it3 = a3[Symbol.iterator]();
  it3.next(); it3.next(); it3.next(); // index 3 after this + done call
  it3.next(); // done path, index 4
  a3.length = 4;
  const r = it3.next(); // 4 >= 4 -> done
  a3.length = 5;
  a3[4] = "four";
  const r2 = it3.next(); // 4 < 5 -> "four"
  __L(3, "boundary => " + r.value + ":" + r.done + " " + r2.value + ":" + r2.done);
}
{
  // done() path keeps incrementing index — 5 done calls then regrow to 8
  const a4 = [1];
  const it4 = a4[Symbol.iterator]();
  it4.next(); // value, index 1
  for (let i = 0; i < 5; i++) it4.next(); // index 6
  a4.length = 8;
  a4[6] = "six";
  const r = it4.next();
  __L(4, "done-increments => " + r.value + ":" + r.done);
}
// iterating an array made slow mid-loop (string prop added) must not disturb values
{
  const a5 = [1, 2, 3];
  const seen = [];
  for (const v of a5) {
    seen.push(v);
    if (seen.length === 1) a5.extra = "slow";
  }
  __L(5, "slow-mid => " + seen.join(",") + " " + a5.extra);
}

summary("arrays_ext");
