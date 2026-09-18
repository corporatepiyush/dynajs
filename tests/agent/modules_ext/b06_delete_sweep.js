// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 start 20", "ok 2 odd-only keys got=[\"k1\",\"k3\",\"k5\",\"k7\",\"k9\",\"k11\",\"k13\",\"k15\",\"k17\",\"k19\"]", "ok 3 values match"];
__EXP[3] = ["delete-future=[1]"];
__EXP[4] = ["delete-self=[5,0]"];
__EXP[5] = ["DONE"];
// modules_ext b06: deletion sweep -- delete every other key BETWEEN keys()/values()
// calls. Each fresh call must see exactly the live set (no stale snapshot reuse).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = {};
for (let i = 0; i < 20; i++) o['k' + i] = i;
ok(Object.keys(o).length === 20, 'start 20');
for (let i = 0; i < 20; i += 2) delete o['k' + i];
ok(JSON.stringify(Object.keys(o)) === JSON.stringify(['k1', 'k3', 'k5', 'k7', 'k9', 'k11', 'k13', 'k15', 'k17', 'k19']), 'odd-only keys got=' + JSON.stringify(Object.keys(o)));
ok(JSON.stringify(Object.values(o)) === JSON.stringify([1, 3, 5, 7, 9, 11, 13, 15, 17, 19]), 'values match');
// delete the REST during a values() getter -- the getter of the FIRST visited key
// deletes all later keys; spec re-check means later keys are skipped (node parity)
const o2 = { a: 1, b: 2, c: 3, d: 4 };
Object.defineProperty(o2, 'a', { get() { delete o2.b; delete o2.c; delete o2.d; return 1; }, enumerable: true, configurable: true });
__L(3, 'delete-future=' + JSON.stringify(Object.values(o2)));
// delete CURRENT key inside its own getter
const o3 = {};
Object.defineProperty(o3, 'z', { get() { delete o3.z; return 5; }, enumerable: true, configurable: true });
o3.y = 0;
__L(4, 'delete-self=' + JSON.stringify(Object.values(o3)));
__L(5, 'DONE');

summary("modules_ext");
