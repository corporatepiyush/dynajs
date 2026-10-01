__EXP = {};
__EXP[1] = ["ok 1 deleted-but-reported key handled, got=[\"a\",\"c\"]", "ok 2 unreported non-configurable -> TypeError", "ok 3 reported non-configurable kept"];
__EXP[3] = ["RESULT all-pass"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }

const t1 = { a: 1, b: 2, c: 3 };
const p1 = new Proxy(t1, {
  ownKeys(t) { delete t.b; return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
});
let r1 = null;
try { r1 = Object.keys(p1); } catch (e) { r1 = 'E:' + e.name; }
ok(r1 === 'E:TypeError' || JSON.stringify(r1) === JSON.stringify(['a', 'b', 'c']) || JSON.stringify(r1) === JSON.stringify(['a', 'c']),
   'deleted-but-reported key handled, got=' + JSON.stringify(r1));

const t2 = {};
Object.defineProperty(t2, 'nc', { value: 1, enumerable: true, configurable: false });
const p2 = new Proxy(t2, {
  ownKeys() { return ['other']; },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
});
ok(ename(() => Object.keys(p2)) === 'TypeError', 'unreported non-configurable -> TypeError');

const t3 = {};
Object.defineProperty(t3, 'nc', { value: 1, enumerable: true, configurable: false });
const p3 = new Proxy(t3, {
  ownKeys(t) { return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
});
ok(JSON.stringify(Object.keys(p3)) === JSON.stringify(['nc']), 'reported non-configurable kept');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

summary("modules_ext");
