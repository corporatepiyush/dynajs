// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// T5: nested loops reusing the same let names — per-iteration copies must not
// alias across nesting levels or iterations.
var fns = [];
for (let i = 0; i < 3; i++) {
  for (let i = 0; i < 2; i++) {
    fns.push(() => "inner" + i);
  }
  fns.push(() => "outer" + i);
}
__A("t05_tdz_nested_reuse.js:a", function () { assert_eq(fns.map(f => f()).join(","), "inner0,inner1,outer0,inner0,inner1,outer1,inner0,inner1,outer2", "a"); });
var log = [];
for (let v = 0; v < 2; v++) {
  let v2 = v;
  { let v = 100 + v2; log.push(String(v)); }
  log.push("h" + v);
}
__A("t05_tdz_nested_reuse.js:b", function () { assert_eq(log.join(","), "100,h0,101,h1", "b"); });
// triple nesting, same name, closures at every level
var acc = [];
for (let a = 0; a < 2; a++) {
  for (let a = 1; a < 2; a++) {
    for (let a = 2; a < 3; a++) {
      acc.push(() => a);
    }
    acc.push(() => a);
  }
  acc.push(() => a);
}
__A("t05_tdz_nested_reuse.js:c", function () { assert_eq(acc.map(f => f()).join(","), "2,1,0,2,1,1", "c"); });
// 1000-iteration loop capturing every 100th iteration + inner reuse
var fns2 = [];
for (let i = 0; i < 1000; i++) {
  for (let i = 0; i < 2; i++) { }
  if (i % 100 === 0) fns2.push(() => i + (1 << 10));
}
__A("t05_tdz_nested_reuse.js:d", function () { assert_eq(fns2.map(f => f()).join(","), "1024,1124,1224,1324,1424,1524,1624,1724,1824,1924", "d"); });

summary("parser_core_ext");
