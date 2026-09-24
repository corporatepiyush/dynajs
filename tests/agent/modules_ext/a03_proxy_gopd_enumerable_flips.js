// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys filters by gOPD enumerability got=[\"a\",\"c\",\"e\"]", "ok 2 values filters likewise got=[1,3,5]", "ok 3 entries likewise got=[[\"a\",1],[\"c\",3],[\"e\",5]]", "ok 4 gopn keeps all 5"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext a03: gOPD trap reports enumerable=true/false per key -- keys() must
// include exactly the enumerable-reported ones. Also checks the KEY-kind snapshot
// trust does not leak non-enumerable-reported keys into the result.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const enumerableBy = { a: true, b: false, c: true, d: false, e: true };
const p = new Proxy({ a: 1, b: 2, c: 3, d: 4, e: 5 }, {
  ownKeys(t) { return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) {
    return { value: t[k], writable: true, enumerable: enumerableBy[String(k)], configurable: true };
  },
});
const keys = Object.keys(p);
ok(JSON.stringify(keys) === JSON.stringify(['a', 'c', 'e']), 'keys filters by gOPD enumerability got=' + JSON.stringify(keys));
const v = Object.values(p);
ok(JSON.stringify(v) === JSON.stringify([1, 3, 5]), 'values filters likewise got=' + JSON.stringify(v));
const e = Object.entries(p);
ok(JSON.stringify(e) === JSON.stringify([['a', 1], ['c', 3], ['e', 5]]), 'entries likewise got=' + JSON.stringify(e));
// gOPN/gOPS (no ENUM_ONLY) must NOT filter
ok(Object.getOwnPropertyNames(p).length === 5, 'gopn keeps all 5');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
