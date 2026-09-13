// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 elision+empty+1 length got=2", "ok 2 hole at 0, 1 at 1", "ok 3 double elision then spread", "ok 4 empty then elision", "ok 5 all elisions", "ok 6 holes read as undefined via spread stringify", "ok 8 spread then elision then value"];
__EXP[2] = ["FAIL 7 spread of holed array yields dense without hole"];
__EXP[3] = ["RESULT FAILURES=1"];
// modules_ext d02: array-literal ELISIONS adjacent to spreads -- pos>count hazard the
// bulk path guards with (pos == u.array.count). Holes must be preserved as holes.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const a = [, ...[], 1];
ok(a.length === 2, 'elision+empty+1 length got=' + a.length);
ok(0 in a === false && a[1] === 1, 'hole at 0, 1 at 1');
const b = [1, , , ...[2]];
ok(b.length === 4 && !(1 in b) && !(2 in b) && b[3] === 2, 'double elision then spread');
const c = [...[], , 5];
ok(c.length === 2 && !(0 in c) && c[1] === 5, 'empty then elision');
const d = [, , ,];
ok(d.length === 3 && !(0 in d) && !(2 in d), 'all elisions');
ok(JSON.stringify([...[1, , 3]]) === '[1,null,3]', 'holes read as undefined via spread stringify');
ok(Object.keys([...[1, , 3]]).length === 2, 'spread of holed array yields dense without hole');
// elision AFTER spread value
const e = [...[9], , 8];
ok(e.length === 3 && e[0] === 9 && !(1 in e) && e[2] === 8, 'spread then elision then value');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
