// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 plain array proto", "ok 2 toStringTag", "ok 3 pushable", "ok 4 descriptors callable", "ok 5 entries pushable", "ok 6 gopn pushable", "ok 7 expando on array got=[\"0\",\"1\",\"2\",\"x\"]"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext c11: values/entries must use a NORMAL empty array each call: the result
// array itself gets no magic -- push() on it works, its prototype is Array.prototype,
// Symbol.toStringTag absent, and holes from sparse sources stay absent.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const v = Object.values({ a: 1 });
ok(Array.isArray(v) && Object.getPrototypeOf(v) === Array.prototype, 'plain array proto');
ok(Object.prototype.toString.call(v) === '[object Array]', 'toStringTag');
v.push(2);
ok(JSON.stringify(v) === '[1,2]', 'pushable');
ok(JSON.stringify(Object.getOwnPropertyDescriptors(v).length === undefined ? v.length : Object.getOwnPropertyDescriptors(v).length), 'descriptors callable'); // smoke: no throw
const e = Object.entries({ a: 1 });
e.push(['b', 2]);
ok(e.length === 2, 'entries pushable');
// result of gOPN/gOPS likewise
const g = Object.getOwnPropertyNames({ z: 1 });
g.push('w');
ok(JSON.stringify(g) === '["z","w"]', 'gopn pushable');
// keys of an object whose only key was added by defineProperty AFTER array became slow
const arr = [1, 2, 3];
arr.x = 'extra';
ok(JSON.stringify(Object.keys(arr)) === '["0","1","2","x"]', 'expando on array got=' + JSON.stringify(Object.keys(arr)));
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
