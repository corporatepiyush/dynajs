// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 n=0 length got=0", "ok 2 n=0 pair contents", "ok 3 n=1 length got=1", "ok 4 n=1 pair contents", "ok 5 n=2 length got=2", "ok 6 n=2 pair contents", "ok 7 n=3 length got=3", "ok 8 n=3 pair contents", "ok 9 n=4 length got=4", "ok 10 n=4 pair contents", "ok 11 n=5 length got=5", "ok 12 n=5 pair contents", "ok 13 n=6 length got=6", "ok 14 n=6 pair contents", "ok 15 n=7 length got=7", "ok 16 n=7 pair contents", "ok 17 n=8 length got=8", "ok 18 n=8 pair contents", "ok 19 n=15 length got=15", "ok 20 n=15 pair contents", "ok 21 n=16 length got=16", "ok 22 n=16 pair contents", "ok 23 n=17 length got=17", "ok 24 n=17 pair contents", "ok 25 fromEntries roundtrip guard", "ok 26 100 keys bulk"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext c06: entries bulk-path size edges: 0,1,2,3,4,5 keys -- the per-pair
// expand_fast_array(2) and the outer expand(len) interact; check length, content,
// Array.isArray on the pairs, and that pair[0]/[1] are dense own index props.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function chk(n) {
  const o = {};
  for (let i = 0; i < n; i++) o['k' + i] = i;
  const e = Object.entries(o);
  ok(e.length === n, 'n=' + n + ' length got=' + e.length);
  let good = true;
  for (let i = 0; i < n; i++) {
    if (!Array.isArray(e[i]) || e[i].length !== 2 || e[i][0] !== 'k' + i || e[i][1] !== i) good = false;
  }
  ok(good, 'n=' + n + ' pair contents');
}
for (const n of [0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17]) chk(n);
// values same boundaries
const v8 = Object.values(Object.fromEntries ? Object.fromEntries([['a', 1]]) : { a: 1 });
ok(JSON.stringify(v8) === '[1]', 'fromEntries roundtrip guard');
// extreme: exactly the presize powers
const o100 = {}; for (let i = 0; i < 100; i++) o100['x' + i] = i;
ok(Object.entries(o100).length === 100 && Object.values(o100).length === 100, '100 keys bulk');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
