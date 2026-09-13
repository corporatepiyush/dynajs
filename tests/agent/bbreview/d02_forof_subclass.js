// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["sub 0:10,1:20,2:30 3 E F"];
__EXP[3] = ["inst true true true"];
__EXP[5] = ["ro 1,2,3 2"];
// D: array subclass + exotic own props through for-of
class Sub extends Array {}
const s = new Sub(10, 20, 30);
s.extra = "E";
s[1.5] = "F";
const out = [];
for (const [i, v] of s.entries()) out.push(i + ":" + v);
__L(0, "sub", out.join(","), s.length, s.extra, s[1.5]);
const arr = [1, 2, 3];
Object.defineProperty(arr, 1, { value: 99, enumerable: false, writable: true, configurable: true });
const out2 = [];
for (const v of arr) out2.push(v);
__A("d02_forof_subclass.js:nenum", function () { assert_eq(out2.join(","), "1,99,3", "nenum"); });
const arr2 = [1, 2, 3];
Object.defineProperty(arr2, 1, { get() { return "GET1"; }, configurable: true });
__A("d02_forof_subclass.js:getter", function () { assert_eq([...arr2].join(","), "1,GET1,3", "getter"); });
__L(3, "inst", s instanceof Array, Array.isArray(s), s.constructor === Sub);
// subclass instance mutated to sparse mid-construction
const s2 = Sub.of(1, 2, 3);
delete s2[1];
__A("d02_forof_subclass.js:subsparse", function () { assert_eq([...s2].map(v => v === undefined ? "H" : v).join(","), "1,H,3", "subsparse"); });
// read-only index during iteration
const frozenIdx = [1, 2, 3];
Object.defineProperty(frozenIdx, 1, { writable: false });
const got = [];
for (const v of frozenIdx) { got.push(v); }
__L(5, "ro", got.join(","), frozenIdx[1]);

summary("bbreview");
