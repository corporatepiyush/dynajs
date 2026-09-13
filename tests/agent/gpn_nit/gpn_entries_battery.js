// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["entries.o1 [[\"0\",\"zero\"],[\"1\",\"one\"],[\"2\",\"two\"],[\"10\",\"ten\"],[\"b\",1],[\"a\",3]]"];
__EXP[1] = ["values.o1 [\"zero\",\"one\",\"two\",\"ten\",1,3]"];
__EXP[2] = ["keys.o1 [\"0\",\"1\",\"2\",\"10\",\"b\",\"a\"]"];
__EXP[3] = ["gpn.o1 [\"0\",\"1\",\"2\",\"10\",\"b\",\"a\"]"];
__EXP[4] = ["entries.arr [[\"0\",10],[\"1\",20],[\"2\",30]]"];
__EXP[5] = ["entries.sparse [[\"0\",1],[\"2\",3],[\"4\",5]]"];
__EXP[6] = ["values.sparse [1,3,5]"];
__EXP[7] = ["keys.sparse [\"0\",\"2\",\"4\"]"];
__EXP[8] = ["gpn.sparse [\"0\",\"2\",\"4\",\"length\"]"];
__EXP[10] = ["entries.big.first [\"0\",0]"];
__EXP[11] = ["entries.big.last [\"4999\",14997]"];
__EXP[15] = ["entries.pairs.first [\"k0\",0]"];
__EXP[16] = ["entries.pairs.last [\"k2999\",2999]"];
__EXP[19] = ["entries.getterDelete [[\"a\",1],[\"b\",\"B\"],[\"c\",3]]"];
__EXP[20] = ["entries.getterFlip [[\"a\",\"A\"],[\"b\",2]]"];
__EXP[21] = ["entries.nonEnum [[\"visible\",2]]"];
__EXP[22] = ["gpn.nonEnum [\"hidden\",\"visible\"]"];
__EXP[24] = ["entries.symbols [[\"s\",1]]"];
__EXP[25] = ["gpnsym.symbols [\"symbol\",\"symbol\"]"];
__EXP[26] = ["entries.str [[\"0\",\"a\"],[\"1\",\"b\"]]"];
__EXP[27] = ["keys.arrctor [\"0\",\"1\",\"2\"]"];
__EXP[28] = ["entries.argslike [[\"0\",7],[\"1\",8]]"];
__EXP[29] = ["entries.frozen [[\"x\",1],[\"y\",2]]"];
__EXP[30] = ["keys.sealed [\"p\",\"q\"]"];
__EXP[31] = ["keys.proxy [\"a\",\"b\"]"];
__EXP[33] = ["entries.proxy [[\"c\",3]]"];
__EXP[34] = ["entries.empty []"];
__EXP[35] = ["entries.one [[\"only\",42]]"];
__EXP[36] = ["values.emptyArr []"];
__EXP[37] = ["entries.oneElemArr [[\"0\",99]]"];
__EXP[38] = ["keys.order [\"1\",\"2\",\"3\",\"z\",\"y\",\"x\"]"];
// GPN2 (Object.entries/values/keys/getOwnPropertyNames/Symbols) battery.
// Regression companion for the bc_read.inc.c GPN2 bulk KEY_AND_VALUE
// expand_fast_array OOM-path key leak fix. Runnable by both dynajs and node;
// output must be byte-identical across engines and against the pre-change
// baseline binary (the leak itself only manifests under OOM, so behavior
// must be unchanged -- this pins that).

function show(tag, v) {
  (typeof print === "function" ? print : console.log)(tag + " => " + JSON.stringify(v));
}

// 1. plain objects: string keys, index-like keys, insertion order
var o1 = { b: 1, 2: "two", a: 3, 0: "zero", 10: "ten", 1: "one" };
__L(0, "entries.o1", Object.entries(o1));
__L(1, "values.o1", Object.values(o1));
__L(2, "keys.o1", Object.keys(o1));
__L(3, "gpn.o1", Object.getOwnPropertyNames(o1));

// 2. arrays, including elisions (holes) -- entries of holes are [i, undefined]
var a1 = [10, 20, 30];
__L(4, "entries.arr", Object.entries(a1));
var a2 = [1, , 3, , 5];
__L(5, "entries.sparse", Object.entries(a2));
__L(6, "values.sparse", Object.values(a2));
__L(7, "keys.sparse", Object.keys(a2));
__L(8, "gpn.sparse", Object.getOwnPropertyNames(a2));

// 3. large array -- exercises the bulk presize (expand_fast_array(ctx, rp, len))
var big = [];
for (var i = 0; i < 5000; i++) big.push(i * 3);
var eb = Object.entries(big);
__A("gpn_entries_battery.js:entries.big.len", function () { assert_eq(eb.length, 5000, "entries.big.len"); });
__L(10, "entries.big.first", eb[0]);
__L(11, "entries.big.last", eb[eb.length - 1]);
__A("gpn_entries_battery.js:keys.big.last", function () { assert_eq(Object.keys(big)[4999], "4999", "keys.big.last"); });
__A("gpn_entries_battery.js:values.big.sum", function () { assert_eq(Object.values(big).reduce(function (x, y) { return x + y; }, 0), 37492500, "values.big.sum"); });

// 4. large pair-array entries -- the bulk KEY_AND_VALUE path (expand_fast_array(ctx, vp, 2))
var eo = {};
for (var i = 0; i < 3000; i++) eo["k" + i] = i;
var ee = Object.entries(eo);
__A("gpn_entries_battery.js:entries.pairs.len", function () { assert_eq(ee.length, 3000, "entries.pairs.len"); });
__L(15, "entries.pairs.first", ee[0]);
__L(16, "entries.pairs.last", ee[ee.length - 1]);
// inner pairs must be real fast arrays of length 2
__A("gpn_entries_battery.js:entries.pairs.innerIsArr", function () { assert_eq(Array.isArray(ee[0]) && ee[0].length === 2, true, "entries.pairs.innerIsArr"); });

// 5. array-of-arrays entries: bulk inner AND bulk outer
var nested = [[1, 2], [3, 4], [5, 6]];
__A("gpn_entries_battery.js:entries.nested", function () { assert_eq(JSON.stringify(Object.entries(nested)), "[[\"0\",[1,2]],[\"1\",[3,4]],[\"2\",[5,6]]]", "entries.nested"); });

// 6. getters: KEY_AND_VALUE runs user code (getter may delete/flip future keys)
var g1 = { a: 1, b: 2, c: 3, d: 4 };
Object.defineProperty(g1, "b", {
  get: function () { delete g1.d; return "B"; },
  enumerable: true, configurable: true
});
__L(19, "entries.getterDelete", Object.entries(g1));

var g2 = { a: 1, b: 2, c: 3 };
Object.defineProperty(g2, "a", {
  get: function () {
    Object.defineProperty(g2, "c", { enumerable: false, configurable: true });
    return "A";
  },
  enumerable: true, configurable: true
});
__L(20, "entries.getterFlip", Object.entries(g2));

// 7. non-enumerable properties are skipped by entries/values/keys, seen by gpn
var ne = {};
Object.defineProperty(ne, "hidden", { value: 1, enumerable: false });
ne.visible = 2;
__L(21, "entries.nonEnum", Object.entries(ne));
__L(22, "gpn.nonEnum", Object.getOwnPropertyNames(ne));
__A("gpn_entries_battery.js:gpnsym.nonEnum", function () { assert_eq(Object.getOwnPropertySymbols(ne).length, 0, "gpnsym.nonEnum"); });

// 8. symbols: never in entries/keys; listed by getOwnPropertySymbols
var sy1 = Symbol("s1");
var so = { s: 1 };
so[sy1] = "sym";
Object.defineProperty(so, Symbol("s2"), { value: "enumsym", enumerable: true });
__L(24, "entries.symbols", Object.entries(so));
__L(25, "gpnsym.symbols", Object.getOwnPropertySymbols(so).map(function (s) { return typeof s; }));

// 9. exotic receivers
__L(26, "entries.str", Object.entries("ab"));
__L(27, "keys.arrctor", Object.keys([4, 5, 6]));
__L(28, "entries.argslike", (function () { return Object.entries(arguments); })(7, 8));

// 10. frozen / sealed / non-extensible
var fz = Object.freeze({ x: 1, y: 2 });
__L(29, "entries.frozen", Object.entries(fz));
var sl = Object.seal({ p: 1, q: 2 });
__L(30, "keys.sealed", Object.keys(sl));

// 11. proxies (ownKeys trap feeds the GPN pipeline)
var px = new Proxy({ a: 1, b: 2 }, {
  ownKeys: function () { return ["a", "b", "0", "zz"]; }
});
try { __L(31, "keys.proxy", Object.keys(px).sort()); } catch (e) { __L(32, "keys.proxy.err", String(e).slice(0, 40)); }
__L(33, "entries.proxy", Object.entries(new Proxy({ c: 3 }, {})));

// 12. single-element and empty (bulk requires len > 0; these use non-bulk or empty loops)
__L(34, "entries.empty", Object.entries({}));
__L(35, "entries.one", Object.entries({ only: 42 }));
__L(36, "values.emptyArr", Object.values([]));
__L(37, "entries.oneElemArr", Object.entries([99]));

// 13. ordering: integer keys ascending first, then strings in insertion order
var ord = {};
ord.z = 1; ord[3] = "i3"; ord.y = 2; ord[1] = "i1"; ord[2] = "i2"; ord.x = 3;
__L(38, "keys.order", Object.keys(ord));
__A("gpn_entries_battery.js:entries.order", function () { assert_eq(JSON.stringify(Object.entries(ord)), "[[\"1\",\"i1\"],[\"2\",\"i2\"],[\"3\",\"i3\"],[\"z\",1],[\"y\",2],[\"x\",3]]", "entries.order"); });

// 14. values with getters producing objects (val stays live across later failures)
var vg = { p: 1, q: 2 };
Object.defineProperty(vg, "q", {
  get: function () { return { deep: true }; },
  enumerable: true, configurable: true
});
__A("gpn_entries_battery.js:values.getterObj", function () { assert_eq(JSON.stringify(Object.values(vg)), "[1,{\"deep\":true}]", "values.getterObj"); });
__A("gpn_entries_battery.js:entries.getterObj", function () { assert_eq(JSON.stringify(Object.entries(vg)), "[[\"p\",1],[\"q\",{\"deep\":true}]]", "entries.getterObj"); });

(typeof print === "function" ? print : console.log)("BATTERY-DONE");

summary("gpn_nit");
