// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["custom-of x", "custom-of y"];
__EXP[6] = ["mutate 1,2,3 10,20,30"];
// D2: for-of fast path must bail when the iterator is NOT the built-in array one
const a = [1, 2, 3];
a[Symbol.iterator] = function* () { yield "x"; yield "y"; };
__A("d06_forof_bail.js:custom-it", function () { assert_eq([...a].join(","), "x,y", "custom-it"); });
for (const v of a) __L(1, "custom-of", v);
// subclass overriding Symbol.iterator
class Sub extends Array {}
Sub.prototype[Symbol.iterator] = function* () { yield "S"; };
__A("d06_forof_bail.js:sub-it", function () { assert_eq([...new Sub(1, 2, 3)].join(","), "S", "sub-it"); });
// nested array iterables sharing the built-in iterator
const nest = [[1, 2], [3, 4]];
const flat = [];
for (const inner of nest) for (const v of inner) flat.push(v);
__A("d06_forof_bail.js:nested", function () { assert_eq(flat.join(","), "1,2,3,4", "nested"); });
// detached typed array for-of
const u = new Uint8Array(3);
const ctl = new Uint8Array(2);
Object.defineProperty(ctl, "buffer", { value: u.buffer });
__A("d06_forof_bail.js:ta", function () { assert_eq([...u].join(","), "0,0,0", "ta"); });
// iterator brand-checked: array-like object with Symbol.iterator stealing Array's
const ali = { length: 2, 0: "a", 1: "b", [Symbol.iterator]: Array.prototype[Symbol.iterator] };
__A("d06_forof_bail.js:stolen", function () { assert_eq([...ali].join(","), "a,b", "stolen"); });
// mutating array content mid-iteration (fast path reads live elements)
const mut = [1, 2, 3];
const got = [];
for (const v of mut) { got.push(v); mut[got.length - 1] = v * 10; }
__L(6, "mutate", got.join(","), mut.join(","));
// for-of over frozen array
const fz = Object.freeze([7, 8]);
__A("d06_forof_bail.js:frozen", function () { assert_eq([...fz].join(","), "7,8", "frozen"); });
// array with hole made during iteration via shift
const sh = [1, 2, 3];
const g2 = [];
for (const v of sh) { g2.push(v === undefined ? "H" : v); sh.shift(); }
__A("d06_forof_bail.js:shift", function () { assert_eq(g2.join(","), "1,3", "shift"); });

summary("bbreview");
