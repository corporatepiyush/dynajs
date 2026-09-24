// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 n=0 concat len+checksum got=0/0", "ok 2 n=1 concat len+checksum got=2/0", "ok 3 n=2 concat len+checksum got=4/2", "ok 4 n=3 concat len+checksum got=6/6", "ok 5 n=4 concat len+checksum got=8/12", "ok 6 n=5 concat len+checksum got=10/20", "ok 7 n=7 concat len+checksum got=14/42", "ok 8 n=8 concat len+checksum got=16/56", "ok 9 n=15 concat len+checksum got=30/210", "ok 10 n=16 concat len+checksum got=32/240", "ok 11 n=17 concat len+checksum got=34/272", "ok 12 n=31 concat len+checksum got=62/930", "ok 13 n=32 concat len+checksum got=64/992", "ok 14 n=33 concat len+checksum got=66/1056", "ok 15 n=63 concat len+checksum got=126/3906", "ok 16 n=64 concat len+checksum got=128/4032", "ok 17 n=65 concat len+checksum got=130/4160", "ok 18 n=255 concat len+checksum got=510/64770", "ok 19 n=256 concat len+checksum got=512/65280", "ok 20 n=257 concat len+checksum got=514/65792", "ok 21 n=1023 concat len+checksum got=2046/1045506", "ok 22 n=1024 concat len+checksum got=2048/1047552", "ok 23 n=1025 concat len+checksum got=2050/1049600", "ok 24 n=1 triple with empties", "ok 25 n=3 triple with empties", "ok 26 n=7 triple with empties", "ok 27 n=63 triple with empties", "ok 28 n=257 triple with empties"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext d03: spread concatenation growth edges: expand_fast_array grows
// geometrically (max(new_len, size*3/2)) -- chain sizes that land exactly on powers
// and 3/2 boundaries; verify content integrity (checksums) after each concat.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function mk(n) { const a = new Array(n); for (let i = 0; i < n; i++) a[i] = i; return a; }
for (const n of [0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 256, 257, 1023, 1024, 1025]) {
  const r = [...mk(n), ...mk(n)];
  let s = 0; for (let i = 0; i < r.length; i++) s += r[i];
  ok(r.length === 2 * n && s === n * (n - 1), 'n=' + n + ' concat len+checksum got=' + r.length + '/' + s);
}
// triple spread with empties interleaved at each size
for (const n of [1, 3, 7, 63, 257]) {
  const r = [...[], ...mk(n), ...[], ...mk(n), ...[], ...mk(n)];
  ok(r.length === 3 * n && r[0] === 0 && r[3 * n - 1] === n - 1, 'n=' + n + ' triple with empties');
}
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
