__EXP = {};
__EXP[1] = ["ok 1 spread entries", "ok 2 flat", "ok 3 destructuring pairs", "ok 4 Map from entries", "ok 5 fromEntries roundtrip", "ok 6 big entries->spread->Map", "ok 7 chained entries spreads"];
__EXP[3] = ["RESULT all-pass"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = { a: 1, b: 2, c: 3 };
ok(JSON.stringify([...Object.entries(o)]) === '[["a",1],["b",2],["c",3]]', 'spread entries');
ok(JSON.stringify(Object.entries(o).flat()) === '["a",1,"b",2,"c",3]', 'flat');
const [p1, p2] = Object.entries(o);
ok(p1[0] === 'a' && p2[1] === 2, 'destructuring pairs');
const m = new Map(Object.entries(o));
ok(m.get('b') === 2, 'Map from entries');
ok(JSON.stringify(Object.fromEntries(Object.entries(o))) === '{"a":1,"b":2,"c":3}', 'fromEntries roundtrip');
const big = {}; for (let i = 0; i < 5000; i++) big['k' + i] = i;
const m2 = new Map([...Object.entries(big)]);
ok(m2.size === 5000 && m2.get('k4999') === 4999, 'big entries->spread->Map');
ok(JSON.stringify([...Object.entries(o), ...Object.entries({ d: 4 })]) === '[["a",1],["b",2],["c",3],["d",4]]', 'chained entries spreads');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
