__EXP = {};
__EXP[1] = ["ok 1 lengths", "ok 2 keys endpoints", "ok 3 values endpoints", "ok 4 entries endpoints", "ok 5 entries midpoint", "ok 6 values checksum got=4999950000", "ok 7 post-delete shift"];
__EXP[3] = ["RESULT all-pass"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const N = 100000;
const o = {};
for (let i = 0; i < N; i++) o['key' + i] = i;
const k = Object.keys(o), v = Object.values(o), e = Object.entries(o);
ok(k.length === N && v.length === N && e.length === N, 'lengths');
ok(k[0] === 'key0' && k[N - 1] === 'key' + (N - 1), 'keys endpoints');
ok(v[0] === 0 && v[N - 1] === N - 1, 'values endpoints');
ok(e[0][0] === 'key0' && e[N - 1][1] === N - 1, 'entries endpoints');
ok(Array.isArray(e[N / 2]) && e[N / 2][0] === 'key' + N / 2, 'entries midpoint');
let s = 0; for (let i = 0; i < N; i++) s += v[i];
ok(s === (N * (N - 1)) / 2, 'values checksum got=' + s);
delete o['key50000'];
ok(Object.keys(o).length === N - 1 && Object.values(o)[50000] === 50001, 'post-delete shift');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
