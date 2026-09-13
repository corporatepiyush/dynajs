// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["b true C"];
__EXP[3] = ["d 10,20 true true false true"];
// T8: class bindings in loops — per-iteration class identity, and the TDZ
// throw for a class binding read before its definition inside the loop.
var classes = [];
for (let i = 0; i < 3; i++) {
  class C { get tag() { return "C" + i; } }
  classes.push(C);
}
__A("t08_tdz_class_loop.js:a", function () { assert_eq(classes.map(C => new C().tag).join(","), "C0,C1,C2", "a"); });
__L(1, "b", classes[0] !== classes[1], classes[0].name);
var log = [];
for (let i = 0; i < 2; i++) {
  try { log.push(typeof D); } catch (e) { log.push("T:" + e.constructor.name); }
  class D { }
  log.push(typeof D);
}
__A("t08_tdz_class_loop.js:c", function () { assert_eq(log.join(","), "T:ReferenceError,function,T:ReferenceError,function", "c"); });
// class extends a per-iteration captured binding; refs kept per iteration
var made = [];
var refs = [];
for (let v of [10, 20]) {
  class B { }
  class D2 extends B { constructor() { super(); this.v = v; } }
  made.push(new D2());
  refs.push({ B: B, D2: D2 });
}
__L(3, "d", made.map(o => o.v).join(","), made[0] instanceof refs[0].B, made[1] instanceof refs[1].B, made[0] instanceof refs[1].B, refs[0].B !== refs[1].B);
// class declared inside a while loop body — fresh binding per iteration
var fns = [];
var w = 0;
while (w < 2) {
  class E { id() { return "e" + w; } }
  const Esave = E;
  fns.push(() => new Esave().id());
  w++;
}
__A("t08_tdz_class_loop.js:e", function () { assert_eq(fns.map(f => f()).join(","), "e2,e2", "e"); });

summary("parser_core_ext");
