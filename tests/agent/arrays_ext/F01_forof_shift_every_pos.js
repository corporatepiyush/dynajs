// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["shift@0 => 1,3,4,5 len=4", "shift@1 => 1,2,4,5 len=4", "shift@2 => 1,2,3,5 len=4", "shift@3 => 1,2,3,4 len=4", "shift@4 => 1,2,3,4,5 len=4", "shift@5 => 1,2,3,4,5 len=5"];
__EXP[1] = ["shift2@0 => 1,4,5,6", "shift2@1 => 1,2,5,6", "shift2@2 => 1,2,3,6", "shift2@3 => 1,2,3,4", "shift2@4 => 1,2,3,4,5", "shift2@5 => 1,2,3,4,5,6", "shift2@6 => 1,2,3,4,5,6"];
__EXP[2] = ["single => 42"];
__EXP[3] = ["nested => o1 a pop=x b c o2 a pop=x b c"];
// F01: for-of array fast path x shift() at EVERY position — length re-check per next()
const out = typeof console !== "undefined" ? console.log : print;

function runShift(pos) {
  const a = [1, 2, 3, 4, 5];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === pos + 1) a.shift();
  }
  return seen.join(",") + " len=" + a.length;
}
for (let p = 0; p < 6; p++) __L(0, "shift@" + p + " => " + runShift(p));

function runShift2(pos) {
  const a = [1, 2, 3, 4, 5, 6];
  const seen = [];
  for (const v of a) {
    seen.push(v);
    if (seen.length === pos + 1) { a.shift(); a.shift(); }
  }
  return seen.join(",");
}
for (let p = 0; p < 7; p++) __L(1, "shift2@" + p + " => " + runShift2(p));

// single element: shift on the very first step
{
  const a = [42];
  const seen = [];
  for (const v of a) { seen.push(v); a.shift(); }
  __L(2, "single => " + seen.join(","));
}
// shift inside nested for-of: inner shrinks, outer unaffected (different arrays)
{
  const log = [];
  for (const o of [1, 2]) {
    log.push("o" + o);
    for (const i of ["a", "b", "c"]) { log.push(i); if (i === "a") log.push("pop=" + ["x", "y"].shift()); }
  }
  __L(3, "nested => " + log.join(" "));
}

summary("arrays_ext");
