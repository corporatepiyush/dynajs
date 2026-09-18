// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[5] = ["c4 1,3 2"];
// B: TDZ elision × closures — per-iteration `let` binding identity
const fns = [];
for (let i = 0; i < 5; i++) {
  const before = () => i;
  fns.push(before);
}
__A("b01_tdz_closures.js:c1", function () { assert_eq(fns.map(f => f()).join(","), "0,1,2,3,4", "c1"); });
const fns2 = [];
for (let j = 0; j < 3; j++) {
  fns2.push(() => j);
  j;
}
__A("b01_tdz_closures.js:c2", function () { assert_eq(fns2.map(f => f()).join(","), "0,1,2", "c2"); });
function f() {
  const g = () => k;
  try { g(); } catch (e) { __A("b01_tdz_closures.js:tdz1", function () { assert_eq(e.constructor.name, "ReferenceError", "tdz1"); }); }
  let k = 1;
  return g();
}
__A("b01_tdz_closures.js:tdz2", function () { assert_eq(f(), 1, "tdz2"); });
const acc = [];
for (let m = 0; m < 4; m++) {
  acc.push(() => m * 10);
  if (m % 2 === 0) { m + 0; }
}
__A("b01_tdz_closures.js:c3", function () { assert_eq(acc.map(g => g()).join(","), "0,10,20,30", "c3"); });
// closure created in iteration 0 reads the mutated loop var — same binding, not a copy
const late = [];
for (let n = 0; n < 3; n++) {
  late.push(() => n);
  n = n + 1;
}
__L(5, "c4", late.map(g => g()).join(","), late.length);
// for-of per-iteration const binding with elision pressure
const ofn = [];
for (const v of [10, 20, 30]) {
  ofn.push(() => v);
}
__A("b01_tdz_closures.js:c5", function () { assert_eq(ofn.map(g => g()).join(","), "10,20,30", "c5"); });

summary("bbreview");
