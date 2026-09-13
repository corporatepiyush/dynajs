// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 frozen keys got=[\"3\",\"5\",\"a\",\"b\"]", "ok 2 frozen values", "ok 3 frozen entries", "ok 4 frozen empty", "ok 5 sealed keys", "ok 6 frozen accessor value got=[7]", "ok 7 frozen idempotent"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b05: keys/values/entries on frozen and sealed objects, incl. frozen
// object with accessor and with index keys. Freeze must not disturb enumerability
// filtering or ordering.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const f = Object.freeze({ a: 1, b: 2, 5: 3, 3: 4 });
ok(JSON.stringify(Object.keys(f)) === '["3","5","a","b"]', 'frozen keys got=' + JSON.stringify(Object.keys(f)));
ok(JSON.stringify(Object.values(f)) === '[4,3,1,2]', 'frozen values');
ok(JSON.stringify(Object.entries(f)) === '[["3",4],["5",3],["a",1],["b",2]]', 'frozen entries');
const g = Object.freeze({});
ok(JSON.stringify(Object.keys(g)) === '[]', 'frozen empty');
const s = Object.seal({ x: 1, y: 2 });
ok(JSON.stringify(Object.keys(s)) === '["x","y"]', 'sealed keys');
// frozen accessor object: values reads the getter
const fa = {};
Object.defineProperty(fa, 'g', { get() { return this.n; }, enumerable: true, configurable: true, n: 7 });
Object.defineProperty(fa, 'n', { value: 7, enumerable: false, configurable: true });
Object.freeze(fa);
ok(JSON.stringify(Object.values(fa)) === '[7]', 'frozen accessor value got=' + JSON.stringify(Object.values(fa)));
// repeated calls on frozen give identical results (idempotent)
ok(JSON.stringify(Object.keys(f)) === JSON.stringify(Object.keys(f)), 'frozen idempotent');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
