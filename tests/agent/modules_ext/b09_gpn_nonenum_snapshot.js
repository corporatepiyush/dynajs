// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys enum-only got=[\"e1\",\"e2\"]", "ok 2 gopn all, insertion order got=[\"e1\",\"n1\",\"e2\",\"n2\"]", "ok 3 values enum-only got=[1,3]", "ok 4 entries enum-only", "ok 5 after flip keys got=[\"e2\"]", "ok 6 all non-enum keys empty", "ok 7 all non-enum values empty", "ok 8 all non-enum entries empty", "ok 9 empty result is a real empty array"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b09: mixed enumerable/non-enumerable with the KEY-kind trust: keys()
// filters on the snapshot's is_enumerable, gOPN keeps everything. Define, redefine
// (data<->accessor), delete, then compare each call's full output. Also length-0
// result on a non-empty object where everything is non-enumerable (bulk len==0 edge
// for the entries/values presize path).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = {};
Object.defineProperty(o, 'e1', { value: 1, enumerable: true, configurable: true });
Object.defineProperty(o, 'n1', { value: 2, enumerable: false, configurable: true });
Object.defineProperty(o, 'e2', { get() { return 3; }, enumerable: true, configurable: true });
Object.defineProperty(o, 'n2', { get() { return 4; }, enumerable: false, configurable: true });
ok(JSON.stringify(Object.keys(o)) === '["e1","e2"]', 'keys enum-only got=' + JSON.stringify(Object.keys(o)));
const gopn = Object.getOwnPropertyNames(o);
ok(gopn.length === 4 && gopn[0] === 'e1' && gopn[1] === 'n1' && gopn[2] === 'e2' && gopn[3] === 'n2', 'gopn all, insertion order got=' + JSON.stringify(gopn));
ok(JSON.stringify(Object.values(o)) === '[1,3]', 'values enum-only got=' + JSON.stringify(Object.values(o)));
ok(JSON.stringify(Object.entries(o)) === '[["e1",1],["e2",3]]', 'entries enum-only');
// flip e1 non-enumerable -> only e2 remains
Object.defineProperty(o, 'e1', { enumerable: false, configurable: true });
ok(JSON.stringify(Object.keys(o)) === '["e2"]', 'after flip keys got=' + JSON.stringify(Object.keys(o)));
// flip everything off -> EMPTY results (len==0 bulk edge)
Object.defineProperty(o, 'e2', { enumerable: false, configurable: true });
ok(JSON.stringify(Object.keys(o)) === '[]', 'all non-enum keys empty');
ok(JSON.stringify(Object.values(o)) === '[]', 'all non-enum values empty');
ok(JSON.stringify(Object.entries(o)) === '[]', 'all non-enum entries empty');
ok(Object.keys(o) instanceof Array && Object.keys(o).length === 0, 'empty result is a real empty array');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
