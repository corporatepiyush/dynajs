// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 proxy spread got=[1,2,3,1,2,3]", "ok 2 length read at least once"];
__EXP[2] = ["FAIL 3 lying length reads proto got=[9,null,\"proto\"]"];
__EXP[3] = ["RESULT FAILURES=1"];
// modules_ext d05: spread of a proxy-wrapped ARRAY -- is_array_iterator fast path may
// still apply (array iterator on proxy with array target? get trap counts). If the
// engine falls to general case, iterator protocol drives; both must produce identical
// element sequences and honor length vs count mismatches.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
const gets = [];
const t = [1, 2, 3];
const p = new Proxy(t, {
  get(o, k, r) { gets.push(String(k)); return Reflect.get(o, k, r); },
});
const r = [...p, ...p];
ok(JSON.stringify(r) === '[1,2,3,1,2,3]', 'proxy spread got=' + JSON.stringify(r));
ok(gets.filter(k => k === 'length').length >= 1, 'length read at least once');
// proxy whose length lies larger than element count (element reads go to prototype)
const p2 = new Proxy([9], { get(o, k) { if (k === 'length') return 3; return Reflect.get(o, k); } });
Object.defineProperty(Array.prototype, 2, { value: 'proto', configurable: true });
try {
  const r2 = [...p2];
  ok(JSON.stringify(r2) === '[9,undefined,"proto"]', 'lying length reads proto got=' + JSON.stringify(r2));
} finally { delete Array.prototype[2]; }
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
