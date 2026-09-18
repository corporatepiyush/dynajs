// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 call with empty spread", "ok 2 call interleaved", "ok 3 call with empty GPN result", "ok 4 call with GPN bulk result", "ok 5 entries flat spread", "ok 6 ctor empty spread", "ok 7 ctor GPN bulk", "ok 8 gpn to gpn spread", "ok 9 Math.max empty", "ok 10 Math.max empty gpn"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext d07: empty spreads in OTHER consumers of the same append machinery:
// call f(...[]), new C(...[]), array concat alternatives, [...x] in template and in
// return position; also spread of Objects.values result (ties GPN bulk to spread bulk).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function f() { return arguments.length; }
ok(f(...[]) === 0, 'call with empty spread');
ok(f(...[], 1, ...[]) === 1, 'call interleaved');
ok(f(...Object.values({})) === 0, 'call with empty GPN result');
ok(f(...Object.values({ a: 1, b: 2 })) === 2, 'call with GPN bulk result');
ok(f(...Object.entries({ a: 1 }).flat()) === 2, 'entries flat spread');
class C { constructor() { this.n = arguments.length; } }
ok((new C(...[])).n === 0, 'ctor empty spread');
ok((new C(...Object.values({ x: 1, y: 2, z: 3 }))).n === 3, 'ctor GPN bulk');
const arr = [...Object.values({ p: 1, q: 2 }), ...Object.keys({ r: 3 }).map(() => 3)];
ok(JSON.stringify(arr) === '[1,2,3]', 'gpn to gpn spread');
ok(Math.max(...[]) === -Infinity, 'Math.max empty');
ok(Math.max(...Object.values({})) === -Infinity, 'Math.max empty gpn');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
