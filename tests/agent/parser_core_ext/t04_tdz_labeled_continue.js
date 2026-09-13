// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// T4: labeled continue over an elided loop head — continue jumping to the
// head/condition re-eval while the elision assumed the only back edge writes
// the loop var first.
var log = [];
outer: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 5; j++) {
    if (j === 1) continue outer;
    if (j === 3) continue;
    log.push(i + ":" + j);
  }
}
__A("t04_tdz_labeled_continue.js:a", function () { assert_eq(log.join(","), "0:0,1:0,2:0", "a"); });
// while-loop whose continue targets the head directly
var log2 = [];
let w = 0;
while (w < 6) {
  w++;
  if (w % 2 === 0) continue;
  log2.push(w + (1 + 1));
}
__A("t04_tdz_labeled_continue.js:b", function () { assert_eq(log2.join(","), "3,5,7", "b"); });
// labeled continue from inside a switch inside a try, over a for-let head
var log3 = [];
outer2: for (let n = 0; n < 4; n++) {
  try {
    switch (n) {
      case 0: log3.push("s0"); continue outer2;
      case 1: log3.push("s1"); break;
      case 2: log3.push("s2"); continue outer2;
      default: log3.push("sd");
    }
    log3.push("after" + n);
  } catch (e) { log3.push("E:" + e.constructor.name); }
}
__A("t04_tdz_labeled_continue.js:c", function () { assert_eq(log3.join(","), "s0,s1,after1,s2,sd,after3", "c"); });
// NOTE: the closure-identity defect when an iteration exits via labeled
// continue is documented in x2_loop_binding_labeled_continue.js (kept FAILing
// there); this file covers the control-flow/value sequences, which match node.
var fns = [];
outer3: for (let p = 0; p < 4; p++) {
  fns.push(p * (1 + 1));
  for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue outer3; }
}
__A("t04_tdz_labeled_continue.js:d", function () { assert_eq(fns.join(","), "0,2,4,6", "d"); });

summary("parser_core_ext");
