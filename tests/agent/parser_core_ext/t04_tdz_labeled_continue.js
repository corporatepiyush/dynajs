__EXP = null;
var log = [];
outer: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 5; j++) {
    if (j === 1) continue outer;
    if (j === 3) continue;
    log.push(i + ":" + j);
  }
}
__A("t04_tdz_labeled_continue.js:a", function () { assert_eq(log.join(","), "0:0,1:0,2:0", "a"); });
var log2 = [];
let w = 0;
while (w < 6) {
  w++;
  if (w % 2 === 0) continue;
  log2.push(w + (1 + 1));
}
__A("t04_tdz_labeled_continue.js:b", function () { assert_eq(log2.join(","), "3,5,7", "b"); });
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
var fns = [];
outer3: for (let p = 0; p < 4; p++) {
  fns.push(p * (1 + 1));
  for (let q = 0; q < 2; q++) { if (q === 1 && p === 2) continue outer3; }
}
__A("t04_tdz_labeled_continue.js:d", function () { assert_eq(fns.join(","), "0,2,4,6", "d"); });

summary("parser_core_ext");
