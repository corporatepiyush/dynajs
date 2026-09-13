// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys excludes symbols", "ok 2 values excludes symbols", "ok 3 entries excludes symbols", "ok 4 gops order", "ok 5 gopn excludes symbols", "ok 6 symbol getter never invoked by ENUM_ONLY sweeps, reads=0", "ok 7 gops lists non-enum symbols too"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b02: symbol keys with ENUM_ONLY. keys/values/entries exclude ALL symbols
// (even enumerable ones); gOPS includes symbols only; gOPN includes strings only.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const s = Symbol('s'), s2 = Symbol('s2');
const o = { a: 1, [s]: 2, b: 3, [s2]: 4 };
ok(JSON.stringify(Object.keys(o)) === '["a","b"]', 'keys excludes symbols');
ok(JSON.stringify(Object.values(o)) === '[1,3]', 'values excludes symbols');
ok(JSON.stringify(Object.entries(o)) === '[["a",1],["b",3]]', 'entries excludes symbols');
ok(JSON.stringify(Object.getOwnPropertySymbols(o).map(String)) === JSON.stringify([String(s), String(s2)]), 'gops order');
ok(JSON.stringify(Object.getOwnPropertyNames(o)) === '["a","b"]', 'gopn excludes symbols');
// symbol with getter must NOT be read by values (no side effect)
let reads = 0;
Object.defineProperty(o, s2, { get() { reads++; return 99; }, enumerable: true, configurable: true });
Object.values(o); Object.keys(o); Object.entries(o);
ok(reads === 0, 'symbol getter never invoked by ENUM_ONLY sweeps, reads=' + reads);
// non-enumerable symbol in gOPS still
Object.defineProperty(o, s, { enumerable: false, configurable: true });
ok(Object.getOwnPropertySymbols(o).length === 2, 'gops lists non-enum symbols too');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
