// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 phantom key in keys got=[\"real\",\"phantom\"]", "ok 2 phantom value in values got=[1,42]", "ok 3 phantom on non-extensible -> TypeError"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext a08: ownKeys reports a key NOT on the target. Extensible target: allowed,
// key participates in keys()/values()/entries() (values must go through the get trap
// with the phantom key; the gOPD filter must call the trap for it). Non-extensible
// target: TypeError.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }

const p1 = new Proxy({ real: 1 }, {
  ownKeys() { return ['real', 'phantom']; },
  getOwnPropertyDescriptor(t, k) {
    if (k === 'phantom') return { value: 42, writable: true, enumerable: true, configurable: true };
    return Reflect.getOwnPropertyDescriptor(t, k);
  },
  get(t, k) { return k === 'phantom' ? 42 : t[k]; },
});
ok(JSON.stringify(Object.keys(p1)) === JSON.stringify(['real', 'phantom']), 'phantom key in keys got=' + JSON.stringify(Object.keys(p1)));
ok(JSON.stringify(Object.values(p1)) === JSON.stringify([1, 42]), 'phantom value in values got=' + JSON.stringify(Object.values(p1)));

const t2 = Object.preventExtensions({ real: 1 });
const p2 = new Proxy(t2, {
  ownKeys() { return ['real', 'phantom']; },
  getOwnPropertyDescriptor(t, k) {
    if (k === 'phantom') return { value: 42, writable: true, enumerable: true, configurable: true };
    return Reflect.getOwnPropertyDescriptor(t, k);
  },
});
ok(ename(() => Object.keys(p2)) === 'TypeError', 'phantom on non-extensible -> TypeError');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
