// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["d one two none"];
// T9: switch with per-case block let bindings; fallthrough into a case whose
// let is still TDZ; closures inside case blocks.
function f(x) {
  var out = [];
  switch (x) {
    case 1: {
      let a = 1 + (2 + 3);
      out.push("a" + a);
    }
    // falls through into case 2's block: b is TDZ there
    case 2: {
      out.push((() => { try { return String(b); } catch (e) { return e.constructor.name; } })());
      let b = 22;
      out.push("b" + b);
      break;
    }
    default: out.push("d");
  }
  return out.join("|");
}
__A("t09_tdz_switch_case_let.js:a", function () { assert_eq(f(1), "a6|ReferenceError|b22", "a"); });
__A("t09_tdz_switch_case_let.js:b", function () { assert_eq(f(2), "ReferenceError|b22", "b"); });
__A("t09_tdz_switch_case_let.js:c", function () { assert_eq(f(3), "d", "c"); });
// let with the same name in two different case blocks
function g(x) {
  switch (x) {
    case 1: { let v = "one"; return v; }
    case 2: { let v = "two"; return v; }
  }
  return "none";
}
__L(3, "d", g(1), g(2), g(9));
// loop+switch: case-block lets re-initialized per iteration, closures capture each
var fns = [];
for (let i = 0; i < 3; i++) {
  switch (i) {
    case 0: { let c0 = i * 10; fns.push(() => c0); break; }
    case 1: { let c1 = i * 20; fns.push(() => c1); break; }
    default: { let cd = i * 30; fns.push(() => cd); }
  }
}
__A("t09_tdz_switch_case_let.js:e", function () { assert_eq(fns.map(fn => fn()).join(","), "0,20,60", "e"); });

summary("parser_core_ext");
