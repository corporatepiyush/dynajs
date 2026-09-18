// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A len=1001 undefs=999 head=head mid@501=mid idx=501 last=501"];
__EXP[1] = ["B drained=[1,3] left=[3]"];
__EXP[2] = ["C 1x6 2x6 3x6 srclen=3"];
__EXP[3] = ["D seen=0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4 guard=31 len=31"];
__EXP[4] = ["E merged=[1,2,null,3,{\"0\":4}] vals=1,2,U,3,4 sum=6 idx=-1 inc4=false frozen=true"];
__EXP[5] = ["F match=true [12,11,10,9,8,7,0,1,2]"];
// X05: for-of x spread x mutators x search — full integration matrix (one divergence catcher)
const out = typeof console !== "undefined" ? console.log : print;

{
  // pipeline: sparse source -> spread (materialize) -> for-of accumulate -> unshift -> search
  const sparse = [];
  sparse.length = 1000;
  sparse[500] = "mid";
  const copy = [...sparse];
  let undefs = 0;
  for (const v of copy) if (v === undefined) undefs++;
  copy.unshift("head");
  __L(0, "A len=" + copy.length + " undefs=" + undefs + " head=" + copy[0] +
      " mid@" + 501 + "=" + copy[501] + " idx=" + copy.indexOf("mid") + " last=" + copy.lastIndexOf("mid"));
}
{
  // for-of over generator spreading INTO an array that for-of then drains via shift
  function* g() { yield* [1, 2, 3]; }
  const a = [...g()];
  const drained = [];
  for (const v of a) { drained.push(v); a.shift(); } // shrinks under the iterator
  __L(1, "B drained=" + JSON.stringify(drained) + " left=" + JSON.stringify([...a]));
}
{
  // self-spread into a fresh array while an outer for-of iterates the ORIGINAL
  const src = [1, 2, 3];
  const log = [];
  for (const v of src) {
    const doubled = [...src, ...src];
    log.push(v + "x" + doubled.length);
    if (log.length > 10) break;
  }
  __L(2, "C " + log.join(" ") + " srclen=" + src.length);
}
{
  // push bulk-append inside a for-of over the SAME array (growth observed by iterator)
  const b = [0];
  const seen = [];
  let guard = 0;
  for (const v of b) {
    seen.push(v);
    if (v < 4) b.push(...[v + 1, v + 1]);
    if (++guard > 50) break;
  }
  __L(3, "D seen=" + seen.join(",") + " guard=" + guard + " len=" + b.length);
}
{
  // concat of everything, then for-of over the concat result, then freeze, then search
  const parts = [[1], [2, , 3], new Uint8Array([4])];
  Array.prototype[2] = "P2";
  let merged;
  try { merged = [].concat(...parts); } finally { delete Array.prototype[2]; }
  let sum = 0;
  const vals = [];
  for (const v of merged) { vals.push(v === undefined ? "U" : v); if (typeof v === "number") sum += v; }
  Object.freeze(merged);
  __L(4, "E merged=" + JSON.stringify(merged) + " vals=" + vals.join(",") + " sum=" + sum +
      " idx=" + merged.indexOf("P2") + " inc4=" + merged.includes(4) + " frozen=" + Object.isFrozen(merged));
}
{
  // slice chain + reverse chain + for-of + spread equality with reference computation
  const base = [];
  for (let i = 0; i < 20; i++) base.push(i);
  let cur = base.slice(5, 15);
  cur.reverse();
  cur = cur.slice(2, 8);
  let acc = [];
  for (const v of cur) acc.push(v);
  acc = [...acc, ...base.slice(0, 3)];
  const ref = base.slice(5, 15).slice().reverse().slice(2, 8);
  const refAll = [...ref, ...base.slice(0, 3)];
  __L(5, "F match=" + (JSON.stringify(acc) === JSON.stringify(refAll)) + " " + JSON.stringify(acc));
}

summary("arrays_ext");
