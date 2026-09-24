// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["c01 50 10"];
__EXP[1] = ["c02 2 1"];
__EXP[4] = ["c05 1 2 100"];
__EXP[6] = ["c07 yes 1 false"];
__EXP[9] = ["c10 true 2"];
// tp15: decl-shape legality — initializers that are not plain constant
// object literals must behave with full dynamic semantics. 10 checks.
// (values here depend on runtime evaluation; folding must not hide them)

// 1: computed member values in the initializer
let dyn = 5;
const D1 = { A: dyn, B: dyn * 2 };
D1.A = 50;
__L(0, "c01", D1.A, D1.B);

// 2: methods in the initializer
const D2 = {
    x: 1,
    f() { return this.x + 1; }
};
__L(1, "c02", D2.f(), D2.x);

// 3: getter in the initializer
const D3 = {
    n: 2,
    get twice() { return this.n * 2; }
};
__A("tp15_decl_shapes.js:c03", function () { assert_eq(D3.twice, 4, "c03"); });
D3.n = 21;
__A("tp15_decl_shapes.js:c04", function () { assert_eq(D3.twice, 42, "c04"); });

// 4: spread in the initializer
const base = { p: 1 };
const D4 = { ...base, q: 2 };
base.p = 100;
__L(4, "c05", D4.p, D4.q, base.p);

// 5: nested object value
const D5 = { inner: { v: 1 } };
D5.inner.v = 11;
__A("tp15_decl_shapes.js:c06", function () { assert_eq(D5.inner.v, 11, "c06"); });

// 6: __proto__ in the initializer
const protoObj = { fromProto: "yes" };
const D6 = { __proto__: protoObj, own: 1 };
__L(6, "c07", D6.fromProto, D6.own, Object.prototype.hasOwnProperty.call(D6, "fromProto"));

// 7: non-const binding with same shape: plain mutable let
let LET = { A: 1 };
LET = { A: 2 };
__A("tp15_decl_shapes.js:c08", function () { assert_eq(LET.A, 2, "c08"); });

// 8: shorthand member (value from a variable)
const shv = 42;
const D8 = { shv };
__A("tp15_decl_shapes.js:c09", function () { assert_eq(D8.shv, 42, "c09"); });

// 9: value from a call
const D9 = { t: Date, u: (1 + 1) };
__L(9, "c10", typeof D9.t === "function", D9.u);

summary("constprop");
