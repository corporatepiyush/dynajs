// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a 0,1,4,9 4"];
// T6: for(;;) infinite loop with a let (re)declared fresh each iteration;
// break out; closures per iteration; TDZ throw inside the headless loop.
var fns = [];
var n = 0;
for (;;) {
  let x = n * n;
  fns.push(() => x);
  n++;
  if (n > 3) break;
}
__L(0, "a", fns.map(f => f()).join(","), n);
// for(;;) where the let is read through a closure BEFORE its declaration runs
var log = [];
var m = 0;
for (;;) {
  const early = () => z;
  if (m === 1) { try { early(); } catch (e) { log.push("T:" + e.constructor.name); } }
  let z = m + 50;
  log.push(String(early()));
  m++;
  if (m > 2) break;
}
__A("t06_tdz_forinf_redeclare.js:b", function () { assert_eq(log.join(","), "50,T:ReferenceError,51,52", "b"); });
// while(true) variant with a shadowing inner let declared per iteration
var out = [];
var k = 0;
while (true) {
  let v = k * 3;
  { let v = k * 100; out.push(String(v)); }
  out.push("v" + v);
  k++;
  if (k > 2) break;
}
__A("t06_tdz_forinf_redeclare.js:c", function () { assert_eq(out.join(","), "0,v0,100,v3,200,v6", "c"); });
// do-while(false) — body executes once, let per iteration, closure survives
var fns2 = [];
var d = 0;
do {
  let y = d + 7;
  fns2.push(() => y);
  d++;
} while (d < 3);
__A("t06_tdz_forinf_redeclare.js:d", function () { assert_eq(fns2.map(f => f()).join(","), "7,8,9", "d"); });

summary("parser_core_ext");
