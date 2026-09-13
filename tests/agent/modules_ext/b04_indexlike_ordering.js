// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys index ordering got=[\"0\",\"1\",\"2\",\"10\",\"4294967294\",\"4294967295\",\"x\",\"2.5\",\"-1\"]", "ok 2 values ordering got=[\"zero\",\"one\",\"two\",\"ten\",\"maxi\",\"max\",\"x\",\"frac\",\"neg\"]", "ok 3 entries ordering", "ok 4 first is index", "ok 5 length 9"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b04: index-like key ordering boundary: 0, 1, 2, 10, 4294967294 are array
// indices (ascending first); '4294967295' (== 2^32-1) is NOT an index -> insertion
// order among strings. keys/values/entries must agree.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = {};
o['10'] = 'ten'; o['4294967295'] = 'max'; o['0'] = 'zero'; o['2'] = 'two'; o['1'] = 'one'; o['x'] = 'x'; o['4294967294'] = 'maxi'; o['2.5'] = 'frac'; o['-1'] = 'neg';
const wantK = ['0', '1', '2', '10', '4294967294', '4294967295', 'x', '2.5', '-1'];
ok(JSON.stringify(Object.keys(o)) === JSON.stringify(wantK), 'keys index ordering got=' + JSON.stringify(Object.keys(o)));
ok(JSON.stringify(Object.values(o)) === JSON.stringify(['zero', 'one', 'two', 'ten', 'maxi', 'max', 'x', 'frac', 'neg']), 'values ordering got=' + JSON.stringify(Object.values(o)));
ok(JSON.stringify(Object.entries(o).map(e => e[0])) === JSON.stringify(wantK), 'entries ordering');
ok(Object.keys(o)[0] === '0', 'first is index');
ok(Object.keys(o).length === 9, 'length 9');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
