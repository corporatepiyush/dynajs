// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["ub1 ReferenceError true"];
__EXP[5] = ["pair 0 a", "pair 1 b"];
__EXP[6] = ["g3 0 100 200 true"];
// B: generator yielding loop var + switch-case inside for(let) + use-before-init
function* gen(n) {
  for (let i = 0; i < n; i++) {
    yield i;
  }
}
__A("b06_tdz_gen_switch.js:g1", function () { assert_eq([...gen(5)].join(","), "0,1,2,3,4", "g1"); });
function* gen2() {
  for (let i = 0; i < 3; i++) {
    const cap = () => i;
    yield cap;
  }
}
__A("b06_tdz_gen_switch.js:g2", function () { assert_eq([...gen2()].map(f => f()).join(","), "0,1,2", "g2"); });
let out = [];
for (let i = 0; i < 6; i++) {
  switch (i % 3) {
    case 0: out.push("z" + i); break;
    case 1: out.push("o" + i); continue;
    default: out.push("d" + i);
  }
  out.push("|" + i);
}
__A("b06_tdz_gen_switch.js:sw", function () { assert_eq(out.join(" "), "z0 |0 o1 d2 |2 z3 |3 o4 d5 |5", "sw"); });
function bad() {
  for (let i = 0; i < 1; i++) {
    try { g(); } catch (e) { __L(3, "ub1", e.constructor.name, e instanceof ReferenceError); }
    let k = 1;
    function g() { return k; }
  }
  return "ok";
}
__A("b06_tdz_gen_switch.js:ub2", function () { assert_eq(bad(), "ok", "ub2"); });
const pairs = [[0, "a"], [1, "b"]];
for (const [n2, s2] of pairs) {
  __L(5, "pair", n2, s2);
}
// generator resumption mid-loop keeps loop var elided-check semantics
function* gen3() {
  for (let i = 0; i < 3; i++) {
    let inner = i * 100;
    yield inner;
  }
}
const it = gen3();
__L(6, "g3", it.next().value, it.next().value, it.next().value, it.next().done);
// try/catch inside loop with TDZ-sensitive read after catch
function tc() {
  let log = [];
  for (let i = 0; i < 3; i++) {
    try {
      if (i === 1) throw new Error("mid");
      log.push("a" + i);
    } catch (e) {
      log.push("c" + i);
    }
    log.push("p" + i);
  }
  return log.join(",");
}
__A("b06_tdz_gen_switch.js:tc1", function () { assert_eq(tc(), "a0,p0,c1,p1,a2,p2", "tc1"); });

summary("bbreview");
