// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["n1 1 undefined false"];
__EXP[3] = ["len2 4294967294 2"];
__EXP[4] = ["edge 7 false"];
__EXP[8] = ["back [1,2,3] 3"];
__EXP[9] = ["ta-bad"];
__EXP[10] = ["ta-range RangeError"];
__REQ = {"dynajs": {"2": 1, "3": 1, "4": 1, "8": 1, "10": 1}, "node": {"2": 1, "3": 1, "4": 1, "8": 1, "9": 1}};
// D: length 2^32-1 edges — O(1), no materialization
const a = [1];
a.length = 2 ** 32 - 1;
__A("d04_forof_len32.js:len", function () { assert_eq(a.length, 4294967295, "len"); });
let count = 0;
for (const v of a) { count++; if (count > 2) break; }
__A("d04_forof_len32.js:iter", function () { assert_eq(count, 3, "iter"); });
const it = a[Symbol.iterator]();
__L(2, "n1", it.next().value, it.next().value, it.next().done);
const b = [];
b.length = 2 ** 32 - 2;
let c = 0;
for (const v of b) { c++; if (c >= 2) break; }
__L(3, "len2", b.length, c);
const d = [7];
d.length = 4294967295;
const it2 = d[Symbol.iterator]();
const r = it2.next();
__L(4, "edge", r.value, r.done);
__A("d04_forof_len32.js:still-alive", function () { assert_eq(1 + 1, 2, "still-alive"); });
try { const e = []; e.length = 2 ** 32; __L(6, "len-err-bad", e.length); } catch (e) { __A("d04_forof_len32.js:len-err", function () { assert_eq(e.constructor.name, "RangeError", "len-err"); }); }
// shrinking a huge-length array back
const f = [1];
f.length = 4294967295;
f.length = 3;
f[1] = 2; f[2] = 3;
__L(8, "back", JSON.stringify([...f]), f.length);
// 2^32-1 length with typed array must still RangeError
try { new Uint8Array(2 ** 32 - 1); __L(9, "ta-bad"); } catch (e) { __L(10, "ta-range", e.constructor.name); }

summary("bbreview");
