// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// T13: TDZ elision under try/finally and exception unwinding inside loops —
// the loop var must be per-iteration even when an iteration aborts via throw.
var fns = [];
for (let i = 0; i < 4; i++) {
  try {
    if (i === 2) throw new Error("mid" + i);
    fns.push(() => "ok" + i);
  } catch (e) {
    fns.push(() => "E:" + e.message);
  } finally {
    if (i === 3) fns.push(() => "fin3");
  }
}
__A("t13_tdz_try_unwind.js:a", function () { assert_eq(fns.map(f => f()).join(","), "ok0,ok1,E:mid2,ok3,fin3", "a"); });
// throw between the per-iteration copy write and its uses; loop restarts
var log = [];
for (let j = 0; j < 3; j++) {
  try {
    let cap = () => j;
    if (j === 1) throw cap;
    log.push("n" + cap());
  } catch (capFn) {
    log.push("thrown:" + capFn());
  }
}
__A("t13_tdz_try_unwind.js:b", function () { assert_eq(log.join(","), "n0,thrown:1,n2", "b"); });
// return-from-loop inside try/finally with captured loop var
function f() {
  var fns = [];
  for (let k = 0; k < 5; k++) {
    try {
      if (k === 2) { fns.push(() => k); return fns; }
      fns.push(() => k * 2);
    } finally { }
  }
  return fns;
}
__A("t13_tdz_try_unwind.js:c", function () { assert_eq(f().map(g => g()).join(","), "0,2,2", "c"); });

summary("parser_core_ext");
