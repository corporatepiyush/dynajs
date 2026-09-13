// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["ins@0 => 1,Y,1,2,3,4,5", "ins@1 => 1,2,Y,2,3,4,5", "ins@2 => 1,2,3,Y,3,4,5", "ins@3 => 1,2,3,4,Y,4,5", "ins@4 => 1,2,3,4,5,Y,5", "del@0 => 1,4,5", "del@1 => 1,2,5", "del@2 => 1,2,3", "del@3 => 1,2,3,4", "del@4 => 1,2,3,4,5", "rep@0 => 1,2,3,4,5", "rep@1 => 1,2,3,4,5", "rep@2 => 1,2,3,4,5", "rep@3 => 1,2,3,4,5", "rep@4 => 1,2,3,4,5"];
__EXP[1] = ["ahead@0 => 1,*,2,3,4", "ahead@1 => 1,2,*,3,4", "ahead@2 => 1,2,3,*,4", "ahead@3 => 1,2,3,4,*"];
__EXP[2] = ["refill => 1,2,n3 final=n1,n2,n3"];
// F03: for-of x splice inserting/removing AT the iteration index, at every position
const out = typeof console !== "undefined" ? console.log : print;

function run(pos, mode) {
  const a = [1, 2, 3, 4, 5];
  const seen = [];
  for (const v of a) {
    seen.push(v === undefined ? "U" : v);
    if (seen.length === pos + 1) {
      if (mode === "ins") a.splice(pos, 0, "X", "Y");
      else if (mode === "del") a.splice(pos, 2);
      else a.splice(pos, 1, "Z");
    }
  }
  return seen.join(",");
}
for (const mode of ["ins", "del", "rep"]) {
  for (let p = 0; p < 5; p++) __L(0, mode + "@" + p + " => " + run(p, mode));
}
// splice inserting AFTER the cursor at each position (shifts future values)
function runAhead(pos) {
  const a = [1, 2, 3, 4];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === pos + 1) a.splice(pos + 1, 0, "*");
  }
  return seen.join(",");
}
for (let p = 0; p < 4; p++) __L(1, "ahead@" + p + " => " + runAhead(p));
// splice that empties the array mid-iteration then refills
{
  const a = [1, 2, 3, 4];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === 2) { a.splice(0, a.length); a.push("n1", "n2", "n3"); }
  }
  __L(2, "refill => " + seen.join(",") + " final=" + a.join(","));
}

summary("arrays_ext");
