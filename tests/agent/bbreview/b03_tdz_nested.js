// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// B: nested loops sharing a name + block re-entry
let total = 0;
for (let i = 0; i < 3; i++) {
  for (let i = 0; i < 3; i++) {
    total += 1;
  }
  total += 100;
}
__A("b03_tdz_nested.js:n1", function () { assert_eq(total, 309, "n1"); });
let out = [];
for (let r = 0; r < 3; r++) {
  {
    let q = r * 2;
    out.push(q);
  }
}
__A("b03_tdz_nested.js:n2", function () { assert_eq(out.join(","), "0,2,4", "n2"); });
const cbs = [];
for (let i = 0; i < 2; i++) {
  for (let j = 0; j < 2; j++) {
    cbs.push(() => i + "-" + j);
  }
}
__A("b03_tdz_nested.js:n3", function () { assert_eq(cbs.map(f => f()).join(" "), "0-0 0-1 1-0 1-1", "n3"); });
let w = 0;
while (true) {
  let t = w * w;
  if (t > 4) { __A("b03_tdz_nested.js:n4", function () { assert_eq(t, 9, "n4"); }); break; }
  w++;
}
// triple nesting with same name at two levels
let t2 = 0;
for (let k = 0; k < 2; k++) {
  for (let k = 0; k < 2; k++) {
    for (let m = 0; m < 2; m++) t2++;
  }
}
__A("b03_tdz_nested.js:n5", function () { assert_eq(t2, 8, "n5"); });
// re-entering a block whose let is captured
const re = [];
for (let i = 0; i < 3; i++) {
  {
    let c = i;
    const f = () => c;
    re.push(f);
  }
}
__A("b03_tdz_nested.js:n6", function () { assert_eq(re.map(f => f()).join(","), "0,1,2", "n6"); });

summary("bbreview");
