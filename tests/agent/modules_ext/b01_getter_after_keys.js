// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 before", "ok 2 keys sees getter key added after: got=[\"a\",\"b\"]", "ok 3 values calls fresh getter: got=[1,2]", "ok 4 entries likewise", "ok 5 non-enum getter excluded got=[\"a\",\"b\"]", "ok 6 data->accessor conversion seen: got=[100,2]", "ok 7 accessor->data conversion seen"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext b01: accessor defined AFTER a keys() call completed. The KEY-kind
// snapshot trust must not memoize anything across calls: a FRESH keys()/values()
// must reflect the object as it is now (getter visible, new key listed).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const o = { a: 1 };
ok(JSON.stringify(Object.keys(o)) === '["a"]', 'before');
Object.defineProperty(o, 'b', { get() { return 2; }, enumerable: true, configurable: true });
ok(JSON.stringify(Object.keys(o)) === '["a","b"]', 'keys sees getter key added after: got=' + JSON.stringify(Object.keys(o)));
ok(JSON.stringify(Object.values(o)) === '[1,2]', 'values calls fresh getter: got=' + JSON.stringify(Object.values(o)));
ok(JSON.stringify(Object.entries(o)) === '[["a",1],["b",2]]', 'entries likewise');
// non-enumerable getter must stay excluded
Object.defineProperty(o, 'h', { get() { return 9; }, enumerable: false, configurable: true });
ok(JSON.stringify(Object.keys(o)) === '["a","b"]', 'non-enum getter excluded got=' + JSON.stringify(Object.keys(o)));
// convert data->accessor in place
Object.defineProperty(o, 'a', { get() { return 100; }, enumerable: true, configurable: true });
ok(JSON.stringify(Object.values(o)) === '[100,2]', 'data->accessor conversion seen: got=' + JSON.stringify(Object.values(o)));
// and back
Object.defineProperty(o, 'a', { value: 1, enumerable: true, configurable: true });
ok(JSON.stringify(Object.values(o)) === '[1,2]', 'accessor->data conversion seen');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
