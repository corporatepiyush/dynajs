// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 sparse keys got=[\"0\",\"2\",\"4\"]", "ok 2 sparse values got=[1,3,5]", "ok 3 sparse entries", "ok 4 far hole keys got=[\"0\",\"1000\"]", "ok 5 length>count excludes phantom indices", "ok 6 post-delete hole"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b10: Object.keys/values/entries on SPARSE arrays: holes are not own
// properties and must be skipped entirely (not undefined, not renumbered). Also on an
// array with trailing length-vs-count mismatch (length set larger).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const arr = [1, , 3, , 5]; // holes at 1,3
ok(JSON.stringify(Object.keys(arr)) === '["0","2","4"]', 'sparse keys got=' + JSON.stringify(Object.keys(arr)));
ok(JSON.stringify(Object.values(arr)) === '[1,3,5]', 'sparse values got=' + JSON.stringify(Object.values(arr)));
ok(JSON.stringify(Object.entries(arr)) === '[["0",1],["2",3],["4",5]]', 'sparse entries');
const big = [];
big[1000] = 'x';
big[0] = 'y';
ok(JSON.stringify(Object.keys(big)) === '["0","1000"]', 'far hole keys got=' + JSON.stringify(Object.keys(big)));
// length manipulated beyond count
const a2 = ['a', 'b'];
a2.length = 5;
ok(JSON.stringify(Object.keys(a2)) === '["0","1"]', 'length>count excludes phantom indices');
// hole after making it dense via assignment
const a3 = [1, 2, 3];
delete a3[1];
ok(JSON.stringify(Object.keys(a3)) === '["0","2"]', 'post-delete hole');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
