// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 string keys got=[\"0\",\"1\"]", "ok 2 string values got=[\"a\",\"b\"]", "ok 3 string entries", "ok 4 number keys", "ok 5 boolean keys", "ok 6 boxed number gopn", "ok 7 null -> TypeError", "ok 8 undefined -> TypeError", "ok 9 values null -> TypeError", "ok 10 entries undefined -> TypeError", "ok 11 gopn null -> TypeError", "ok 12 symbol receiver gopn"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b07: JS_ToObject coercion of primitives for keys/values/entries/gOPN:
// strings index like arrays, numbers/booleans get no own keys, null/undefined throw
// TypeError. Error NAMES only.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }
ok(JSON.stringify(Object.keys('ab')) === '["0","1"]', 'string keys got=' + JSON.stringify(Object.keys('ab')));
ok(JSON.stringify(Object.values('ab')) === '["a","b"]', 'string values got=' + JSON.stringify(Object.values('ab')));
ok(JSON.stringify(Object.entries('ab')) === '[["0","a"],["1","b"]]', 'string entries');
ok(JSON.stringify(Object.keys(42)) === '[]', 'number keys');
ok(JSON.stringify(Object.keys(true)) === '[]', 'boolean keys');
ok(JSON.stringify(Object.getOwnPropertyNames(new Number(5))) === '[]', 'boxed number gopn');
ok(ename(() => Object.keys(null)) === 'TypeError', 'null -> TypeError');
ok(ename(() => Object.keys(undefined)) === 'TypeError', 'undefined -> TypeError');
ok(ename(() => Object.values(null)) === 'TypeError', 'values null -> TypeError');
ok(ename(() => Object.entries(undefined)) === 'TypeError', 'entries undefined -> TypeError');
ok(ename(() => Object.getOwnPropertyNames(null)) === 'TypeError', 'gopn null -> TypeError');
// symbol receiver is fine (own props of Symbol wrapper)
ok(JSON.stringify(Object.getOwnPropertyNames(Object(Symbol()))) === '[]', 'symbol receiver gopn');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
