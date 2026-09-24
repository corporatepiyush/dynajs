// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 6 numeric ownKeys entry -> TypeError", "ok 7 object ownKeys entry -> TypeError"];
__EXP[2] = ["FAIL 1 keys order got=[\"b\",\"0\",\"z\",\"2\",\"a\",\"10\",\"1\"]", "FAIL 2 values order got=[7,5,1,4,3,2,8]", "FAIL 3 entries order got=[\"b\",\"0\",\"z\",\"2\",\"a\",\"10\",\"1\"]", "FAIL 4 gopn order", "FAIL 5 gops order"];
__EXP[3] = ["RESULT FAILURES=5"];
// modules_ext a10: key ORDER through a proxy: integer-like keys ascending first, then
// strings in trap-report order, symbols last. keys/values/entries/gOPN/gOPS order parity.
// Also: ownKeys trap returning a non-string/symbol must throw TypeError (name parity).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }
const s1 = Symbol('s1'), s2 = Symbol('s2');
const t = { z: 1, 10: 2, a: 3, 2: 4, 0: 5, [s1]: 6, b: 7, 1: 8, [s2]: 9 };
// trap reports in a SCRAMBLED order: the engine must re-sort per spec
const p = new Proxy(t, {
  ownKeys(x) { return ['b', '0', s2, 'z', '2', s1, 'a', '10', '1']; },
  getOwnPropertyDescriptor(x, k) { return Reflect.getOwnPropertyDescriptor(x, k); },
  get(x, k) { return x[k]; },
});
const want = ['0', '1', '2', '10', 'z', 'a', 'b'];
ok(JSON.stringify(Object.keys(p)) === JSON.stringify(want), 'keys order got=' + JSON.stringify(Object.keys(p)));
ok(JSON.stringify(Object.values(p)) === JSON.stringify([5, 8, 4, 2, 1, 3, 7]), 'values order got=' + JSON.stringify(Object.values(p)));
ok(JSON.stringify(Object.entries(p).map(x => x[0])) === JSON.stringify(want), 'entries order got=' + JSON.stringify(Object.entries(p).map(x => x[0])));
ok(JSON.stringify(Object.getOwnPropertyNames(p)) === JSON.stringify(want), 'gopn order');
ok(JSON.stringify(Object.getOwnPropertySymbols(p).map(String)) === JSON.stringify([String(s1), String(s2)]), 'gops order');
// ownKeys trap result validation: numbers and objects are invalid
const pn = new Proxy({}, { ownKeys() { return [2]; }, getOwnPropertyDescriptor(x, k) { return Reflect.getOwnPropertyDescriptor(x, k); } });
ok(ename(() => Object.keys(pn)) === 'TypeError', 'numeric ownKeys entry -> TypeError');
const po = new Proxy({}, { ownKeys() { return [{}]; }, getOwnPropertyDescriptor(x, k) { return Reflect.getOwnPropertyDescriptor(x, k); } });
ok(ename(() => Object.keys(po)) === 'TypeError', 'object ownKeys entry -> TypeError');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
