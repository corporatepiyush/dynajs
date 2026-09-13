// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys invariant lie -> TypeError", "ok 2 values invariant lie -> TypeError", "ok 3 entries invariant lie -> TypeError", "ok 4 enumerability lie on non-config -> TypeError"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext a09: gOPD trap reports a NON-CONFIGURABLE target key as configurable ->
// TypeError must come from the gOPD filter inside GPN (invariant check parity), for
// keys(), values() and entries() alike. Error NAME printed, never the message.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }

const t = {};
Object.defineProperty(t, 'nc', { value: 7, enumerable: true, configurable: false });
const p = new Proxy(t, {
  ownKeys(x) { return Reflect.ownKeys(x); },
  getOwnPropertyDescriptor(x, k) {
    const d = Reflect.getOwnPropertyDescriptor(x, k);
    if (d && k === 'nc') return { value: d.value, writable: true, enumerable: true, configurable: true }; // lie
    return d;
  },
  get(x, k) { return x[k]; },
});
ok(ename(() => Object.keys(p)) === 'TypeError', 'keys invariant lie -> TypeError');
ok(ename(() => Object.values(p)) === 'TypeError', 'values invariant lie -> TypeError');
ok(ename(() => Object.entries(p)) === 'TypeError', 'entries invariant lie -> TypeError');
// non-enumerable lie on a non-configurable enumerable key is also a violation
const p2 = new Proxy(t, {
  ownKeys(x) { return Reflect.ownKeys(x); },
  getOwnPropertyDescriptor(x, k) {
    const d = Reflect.getOwnPropertyDescriptor(x, k);
    if (d && k === 'nc') return { value: d.value, writable: true, enumerable: false, configurable: false };
    return d;
  },
});
ok(ename(() => Object.keys(p2)) === 'TypeError', 'enumerability lie on non-config -> TypeError');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
