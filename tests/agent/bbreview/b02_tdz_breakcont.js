// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~rc1", "~rc1"]];
// B: continue/break/return after the elided TDZ check
function sumUntil(limit) {
  let s = 0;
  for (let i = 0; i < 100; i++) {
    s += i;
    if (i === limit) return s;
    if (i % 2 === 1) continue;
  }
  return s;
}
__L(0, "rc1", sumUntil(4), sumUntil(-1));
let out = [];
outer: for (let a = 0; a < 3; a++) {
  for (let b = 0; b < 3; b++) {
    if (b === 1) continue;
    if (b === 2) break outer;
    out.push(a * 10 + b);
  }
}
__A("b02_tdz_breakcont.js:bc2", function () { assert_eq(out.join(","), "0", "bc2"); });
for (let z = 0; z < 2; z++) {
  const w = z;
  if (w === 0) break;
  var _unused = 1;
}
try { __L(2, "bc3-bad", z); } catch (e) { __A("b02_tdz_breakcont.js:bc3", function () { assert_eq(e.constructor.name, "ReferenceError", "bc3"); }); }
let cnt = 0;
lab: for (let p = 0; p < 5; p++) {
  cnt += p;
  continue lab;
}
__A("b02_tdz_breakcont.js:bc4", function () { assert_eq(cnt, 10, "bc4"); });
// labeled continue on inner loop with elided loop var
let tot = 0;
out1: for (let u = 0; u < 4; u++) {
  for (let v = 0; v < 4; v++) {
    if (v > u) continue out1;
    tot += u * 10 + v;
  }
}
__A("b02_tdz_breakcont.js:bc5", function () { assert_eq(tot, 210, "bc5"); });
// return from inside nested loop after elided checks
function early() {
  for (let i = 0; i < 5; i++) {
    for (let j = 0; j < 5; j++) {
      if (i * j === 6) return i + ":" + j;
    }
  }
  return "none";
}
__A("b02_tdz_breakcont.js:bc6", function () { assert_eq(early(), "2:3", "bc6"); });

summary("bbreview");
