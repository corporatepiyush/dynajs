// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["pop@0 => 1,2,3,4 len=4", "pop@1 => 1,2,3,4 len=4", "pop@2 => 1,2,3,4 len=4", "pop@3 => 1,2,3,4 len=4", "pop@4 => 1,2,3,4,5 len=4", "pop@5 => 1,2,3,4,5 len=5"];
__EXP[1] = ["grow-pop => 1,2,3 final=1,2,3"];
__EXP[2] = ["drain => 1,2 guard=2 len=1"];
// F02: for-of x pop() at EVERY position — iteration ends when length shrinks below cursor
const out = typeof console !== "undefined" ? console.log : print;

function runPop(pos) {
  const a = [1, 2, 3, 4, 5];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === pos + 1) a.pop();
  }
  return seen.join(",") + " len=" + a.length;
}
for (let p = 0; p < 6; p++) __L(0, "pop@" + p + " => " + runPop(p));

// pop pushed onto the array being iterated (grow-then-shrink interplay)
{
  const a = [1, 2];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === 1) a.push(3, 4);
    if (seen.length === 3) a.pop();
  }
  __L(1, "grow-pop => " + seen.join(",") + " final=" + a.join(","));
}
// pop until empty while iterating — loop must terminate, no infinite done-false
{
  const a = [1, 2, 3];
  const seen = [];
  let guard = 0;
  for (const v of a) {
    seen.push(v);
    a.pop();
    if (++guard > 50) break;
  }
  __L(2, "drain => " + seen.join(",") + " guard=" + guard + " len=" + a.length);
}

summary("arrays_ext");
