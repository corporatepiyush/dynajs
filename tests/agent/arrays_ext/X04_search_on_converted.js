// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A merged=[1,null,3,3,4,{\"0\":5,\"1\":6}] idx3=2 last3=3 inc6=false len=6"];
__EXP[1] = ["B spread-search => 1 3 true"];
__EXP[2] = ["C slice-search => 5 true 6 len=7"];
__EXP[3] = ["D ta => 1 3 true false fromneg=3 1"];
__EXP[4] = ["E splice-search => [1,3,4] 2 -1"];
__EXP[5] = ["F rev-search => [5,4,3,2,1] 0 4"];
__EXP[6] = ["G unshift-search => [1,2,3,4] 3 0"];
// X04: search fast paths over CONVERTED results (concat/slice/spread outputs feeding
// indexOf/includes/lastIndexOf in one pipeline), typed arrays included
const out = typeof console !== "undefined" ? console.log : print;

{
  const holey = [1, , 3];
  const dense = [3, 4];
  const ta = new Uint8Array([5, 6]);
  const merged = [].concat(holey, dense, ta);
  __L(0, "A merged=" + JSON.stringify(merged) + " idx3=" + merged.indexOf(3) +
      " last3=" + merged.lastIndexOf(3) + " inc6=" + merged.includes(6) + " len=" + merged.length);
}
{
  // spread result (fast dense) -> search
  const s = [...[9, 8, 7, 8]];
  __L(1, "B spread-search => " + s.indexOf(8) + " " + s.lastIndexOf(8) + " " + s.includes(7));
}
{
  // slice of sparse (holey result) -> search with proto
  Array.prototype[5] = "PX";
  try {
    const sp = [];
    sp.length = 8;
    sp[6] = "six";
    const c = sp.slice(0, 7);
    __L(2, "C slice-search => " + c.indexOf("PX") + " " + c.includes("PX") + " " +
        c.indexOf("six") + " len=" + c.length);
  } finally { delete Array.prototype[5]; }
}
{
  // typed array searches (own fast path family)
  const u = new Uint8Array([1, 2, 3, 2]);
  __L(3, "D ta => " + u.indexOf(2) + " " + u.lastIndexOf(2) + " " + u.includes(3) + " " + u.includes(9) +
      " fromneg=" + u.indexOf(2, -2) + " " + u.lastIndexOf(2, -3));
}
{
  // slow array converted by named props, searched, then spliced and searched again
  const e = [1, 2, 3, 4];
  e.k = "K";
  e.splice(1, 1);
  __L(4, "E splice-search => " + JSON.stringify([...e]) + " " + e.indexOf(4) + " " + e.indexOf(2));
}
{
  // reverse of concat result then search (double conversion round trip)
  const f = [1, 2, 3].concat([4, 5]);
  f.reverse();
  __L(5, "F rev-search => " + JSON.stringify(f) + " " + f.indexOf(5) + " " + f.lastIndexOf(1));
}
{
  // unshift onto spread result then lastIndexOf
  const g = [...[3, 4]];
  g.unshift(1, 2);
  __L(6, "G unshift-search => " + JSON.stringify(g) + " " + g.lastIndexOf(4) + " " + g.indexOf(1));
}

summary("arrays_ext");
