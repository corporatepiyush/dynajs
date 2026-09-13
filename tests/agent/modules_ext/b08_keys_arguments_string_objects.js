// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 arguments keys got=[\"0\",\"1\",\"2\",\"extra\"]", "ok 2 function length non-enum excluded", "ok 3 function name non-enum excluded", "ok 4 own prop included", "ok 5 date keys got=[\"x\"]", "ok 6 regexp: y in, lastIndex non-enum out: got=[\"y\"]", "ok 7 error own prop in", "ok 8 plain error keys no own props except stack class"];
__EXP[3] = ["stack-enum=false"];
__EXP[4] = ["DONE"];
// modules_ext b08: exotic but plain-property objects: arguments object, function,
// RegExp, Date, Error -- keys() ordering and enumerability (non-enum props like
// .length on functions are excluded; error .stack enumerability parity matters).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function args() { return arguments; }
const a = args(1, 2, 3);
a.extra = 4;
ok(JSON.stringify(Object.keys(a)) === '["0","1","2","extra"]', 'arguments keys got=' + JSON.stringify(Object.keys(a)));
const fn = function (p, q) {};
fn.own = 1;
ok(!Object.keys(fn).includes('length'), 'function length non-enum excluded');
ok(!Object.keys(fn).includes('name'), 'function name non-enum excluded');
ok(Object.keys(fn).includes('own'), 'own prop included');
const d = new Date(0); d.x = 1;
ok(JSON.stringify(Object.keys(d)) === '["x"]', 'date keys got=' + JSON.stringify(Object.keys(d)));
const r = /ab/g; r.y = 2;
const rk = Object.keys(r);
ok(rk.includes('y') && !rk.includes('lastIndex'), 'regexp: y in, lastIndex non-enum out: got=' + JSON.stringify(rk));
const e = new TypeError('m'); e.z = 3;
const ek = Object.keys(e);
ok(ek.includes('z'), 'error own prop in');
__L(3, 'stack-enum=' + ek.includes('stack')); // enumerable .stack is a known engine divergence point; runner compares
ok(JSON.stringify(Object.keys(new Error('x'))).indexOf('z') === -1, 'plain error keys no own props except stack class');
__L(4, 'DONE');

summary("modules_ext");
