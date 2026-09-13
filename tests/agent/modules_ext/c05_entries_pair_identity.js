// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 pairs distinct", "ok 2 fresh arrays per call", "ok 3 second result unaffected: [[\"a\",1],[\"b\",2],[\"c\",3]]", "ok 4 object unaffected", "ok 5 bulk lengths", "ok 6 bulk pair identity", "ok 7 bulk isolation got=5000", "ok 8 bulk order endpoints"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext c05: entries pair-array identity isolation. Mutating a returned pair
// must not affect any other pair, a later pair, or a re-run; the top array must be
// fresh each call. Bulk path (large object) included.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = { a: 1, b: 2, c: 3 };
const e1 = Object.entries(o);
ok(e1[0] !== e1[1] && e1[1] !== e1[2] && e1[0] !== e1[2], 'pairs distinct');
const e2 = Object.entries(o);
ok(e1 !== e2 && e1[0] !== e2[0], 'fresh arrays per call');
e1[0][0] = 'HACK';
e1[1][1] = 999;
ok(JSON.stringify(e2) === '[["a",1],["b",2],["c",3]]', 'second result unaffected: ' + JSON.stringify(e2));
ok(o.a === 1 && o.b === 2, 'object unaffected');
// bulk path: 10000 pairs
const big = {};
for (let i = 0; i < 10000; i++) big['k' + i] = i;
const eb1 = Object.entries(big);
const eb2 = Object.entries(big);
ok(eb1.length === 10000 && eb2.length === 10000, 'bulk lengths');
ok(eb1[0] !== eb2[0] && eb1[9999] !== eb2[9999], 'bulk pair identity');
eb1[5000][1] = -1;
ok(eb2[5000][1] === 5000 && big.k5000 === 5000, 'bulk isolation got=' + eb2[5000][1]);
ok(JSON.stringify(eb1[0]) === '["k0",0]' && JSON.stringify(eb1[9999]) === '["k9999",9999]', 'bulk order endpoints');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
