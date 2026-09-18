// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A throw=TypeError calls=2 len=5 multiset=[1,2,3,4,5]"];
__EXP[1] = ["B throw=RangeError calls=3 len=5 multiset=[1,2,7,8,9] tag=T"];
__EXP[2] = ["C proto-seen=true done own1=true"];
__EXP[3] = ["D parity=true [1,2,3,5,7,8,9] len=7"];
__EXP[4] = ["E default=[1,10,\"a\",null] len=4"];
__EXP[5] = ["F mutate-cmp len=4 content=[1,2,3,4] (impl-defined state)"];
// X03: sort on converted slow arrays with a mid-sort THROWING comparator.
// Spec: the array is left in an IMPLEMENTATION-DEFINED order after the abort — so we assert
// the exception, that no elements are LOST (multiset preserved), and length.
// (Byte-level array state differences vs node are impl-defined, not bugs.)
const out = typeof console !== "undefined" ? console.log : print;

function sortedJoin(a) { return JSON.stringify([...a].sort((x, y) => (x > y ? 1 : x < y ? -1 : 0))); }

{
  // dense fast array, comparator throws at call 2
  const a = [3, 1, 2, 5, 4];
  let calls = 0;
  let r;
  try {
    a.sort((x, y) => { if (++calls === 2) throw new TypeError("CMP"); return x - y; });
    r = "NO-THROW";
  } catch (e) { r = e.name; }
  __L(0, "A throw=" + r + " calls=" + calls + " len=" + a.length + " multiset=" + sortedJoin(a));
}
{
  // slow (dictionary) array, comparator throws at call 3
  const b = [9, 7, 8, 1, 2];
  b.tag = "T";
  b[1000000] = "far";
  b.length = 5;
  let calls = 0;
  let r;
  try {
    b.sort((x, y) => { if (++calls === 3) throw new RangeError("CMP2"); return x - y; });
    r = "NO-THROW";
  } catch (e) { r = e.name; }
  __L(1, "B throw=" + r + " calls=" + calls + " len=" + b.length + " multiset=" + sortedJoin(b) + " tag=" + b.tag);
}
{
  // holey array + proto at the hole: sort READS proto values into comparisons (Get semantics)
  Array.prototype[1] = "P1";
  try {
    const c = [3, , 1, 2];
    let sawProto = false;
    let r;
    try {
      c.sort((x, y) => { if (x === "P1" || y === "P1") sawProto = true; return (x > y ? 1 : x < y ? -1 : 0); });
      r = "done";
    } catch (e) { r = e.name; }
    __L(2, "C proto-seen=" + sawProto + " " + r + " own1=" + Object.prototype.hasOwnProperty.call(c, 1));
  } finally { delete Array.prototype[1]; }
}
{
  // clean runs: fast sort vs converted-slow sort produce IDENTICAL orders
  const mkFast = () => [5, 3, 8, 1, 9, 2, 7];
  const mkSlow = () => { const a = mkFast(); a[100000] = "x"; a.length = 7; a.x = 1; return a; };
  const rf = mkFast().sort((x, y) => x - y);
  const rs = mkSlow().sort((x, y) => x - y);
  __L(3, "D parity=" + (JSON.stringify(rf) === JSON.stringify(rs)) + " " + JSON.stringify(rf) + " len=" + rs.length);
}
{
  // numeric/string mixed default sort (string conversion) on a slow array
  const d = [10, 1, "a", , 2];
  d.z = 1;
  d[100000] = 0;
  d.length = 4;
  const r = d.sort();
  __L(4, "E default=" + JSON.stringify(r) + " len=" + r.length);
}
{
  // sort with a comparator that MUTATES the array (fast path hazard) — no throw, valid prefix
  const e = [4, 2, 3, 1];
  let mutated = false;
  e.sort((x, y) => { if (!mutated) { mutated = true; e.length = 2; } return x - y; });
  __L(5, "F mutate-cmp len=" + e.length + " content=" + JSON.stringify(e) + " (impl-defined state)");
}

summary("arrays_ext");
