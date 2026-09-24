// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["dw2 0 1"];
__EXP[3] = ["dw3 1 0 1"];
__EXP[4] = ["ndw 6 3"];
// B: do..while with let + for(;;) with inner redeclared let
let out = [];
let n = 0;
do {
  let v = n * n;
  out.push(v);
  n++;
} while (n < 4);
__A("b05_tdz_dowhile.js:dw", function () { assert_eq(out.join(","), "0,1,4,9", "dw"); });
let k = 3;
do {
  var dummy = k;
  k--;
} while (k > 0);
__L(1, "dw2", k, dummy);
let acc = [];
for (;;) {
  let t = acc.length;
  acc.push(t * t);
  if (acc.length === 5) break;
}
__A("b05_tdz_dowhile.js:f1", function () { assert_eq(acc.join(","), "0,1,4,9,16", "f1"); });
const cbs = [];
let d = 0;
do {
  let c = d;
  cbs.push(() => c);
  d++;
} while (false);
__L(3, "dw3", cbs.length, cbs[0](), d);
let total = 0, i = 0;
do {
  let j = 0;
  do {
    let jj = i;
    total += jj;
    j++;
  } while (j < 2);
  i++;
} while (i < 3);
__L(4, "ndw", total, i);
// do..while with TDZ use-before-init of a different let — must still throw
function bad() {
  do {
    try { h(); } catch (e) { __A("b05_tdz_dowhile.js:dw4", function () { assert_eq(e.constructor.name, "ReferenceError", "dw4"); }); }
    let kk = 1;
    function h() { return kk; }
  } while (false);
  return "ok";
}
__A("b05_tdz_dowhile.js:dw5", function () { assert_eq(bad(), "ok", "dw5"); });

summary("bbreview");
