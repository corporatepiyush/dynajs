// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["i32-stable: 3,1,2,5,4 calls>0=true"];
__EXP[2] = ["u32-default: 0,1,2147483648,4294967295"];
__EXP[3] = ["u32-cmp: 0,1,4294967295"];
__EXP[4] = ["bi64: 1,5,7,10"];
__EXP[5] = ["bi64-desc: 10,7,5,1"];
__EXP[6] = ["DONE"];
// modules_ext f08: Int32Array with a NaN-returning comparator: ints can never BE NaN,
// so the comparator MUST be consulted (no NaN short-circuit possible) and a NaN
// result means +0 -> stable order preserved. Also BigInt64Array guard.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const i = new Int32Array([3, 1, 2, 5, 4]);
let calls = 0;
i.sort(function (a, b) { calls++; return NaN; });
__L(1, 'i32-stable: ' + i.join(',') + ' calls>0=' + (calls > 0));
const u = new Uint32Array([4294967295, 0, 2147483648, 1]);
u.sort();
__L(2, 'u32-default: ' + u.join(','));
const u2 = new Uint32Array([4294967295, 0, 1]);
u2.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
__L(3, 'u32-cmp: ' + u2.join(','));
if (typeof BigInt === 'function') {
  const b = new BigInt64Array([10n, 5n, 7n, 1n]);
  b.sort();
  __L(4, 'bi64: ' + b.join(','));
  b.sort((x, y) => (x > y ? -1 : x < y ? 1 : 0));
  __L(5, 'bi64-desc: ' + b.join(','));
}
__L(6, 'DONE');

summary("modules_ext");
