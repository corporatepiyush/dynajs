// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 gopn mask+order got=[\"1\",\"2\",\"3\",\"b\",\"a\"]", "ok 2 gops mask", "ok 3 keys order got=[\"1\",\"2\",\"3\",\"b\",\"a\"]", "ok 4 values order got=[4,5,2,1,3]"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b03: STRING_MASK vs SYMBOL_MASK via gOPN/gOPS on a mixed object with
// index-like keys -- integer indices sort ascending FIRST in both, strings after,
// gOPS never contains strings, gOPN never contains symbols.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const sx = Symbol('x');
const o = { [sx]: 0, b: 1, 3: 2, a: 3, 1: 4, 2: 5 };
ok(JSON.stringify(Object.getOwnPropertyNames(o)) === '["1","2","3","b","a"]', 'gopn mask+order got=' + JSON.stringify(Object.getOwnPropertyNames(o)));
ok(JSON.stringify(Object.getOwnPropertySymbols(o).map(String)) === JSON.stringify(['Symbol(x)']), 'gops mask');
// keys/values/entries (STRING_MASK | ENUM_ONLY) same order
ok(JSON.stringify(Object.keys(o)) === '["1","2","3","b","a"]', 'keys order got=' + JSON.stringify(Object.keys(o)));
ok(JSON.stringify(Object.values(o)) === '[4,5,2,1,3]', 'values order got=' + JSON.stringify(Object.values(o)));
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
