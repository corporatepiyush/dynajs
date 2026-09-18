// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ok 1 keys on revoked -> TypeError", "ok 2 values on revoked -> TypeError", "ok 3 entries on revoked -> TypeError", "ok 4 gopn on revoked -> TypeError", "ok 5 revoke inside ownKeys -> TypeError"];
__EXP[3] = ["RESULT all-pass"];
// modules_ext a06: revocation. Revoke inside ownKeys trap (mid-GPN) and after keys()
// completed; TypeError name parity. Also values()/entries() on a revoked proxy.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) __L(1, 'ok ' + T + ' ' + m); else { F++; __L(2, 'FAIL ' + T + ' ' + m); } }
async function enameP(p) { try { await p; return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }

const { proxy: pa, revoke: ra } = Proxy.revocable({ a: 1, b: 2 }, {});
ra();
ok(ename(() => Object.keys(pa)) === 'TypeError', 'keys on revoked -> TypeError');
ok(ename(() => Object.values(pa)) === 'TypeError', 'values on revoked -> TypeError');
ok(ename(() => Object.entries(pa)) === 'TypeError', 'entries on revoked -> TypeError');
ok(ename(() => Object.getOwnPropertyNames(pa)) === 'TypeError', 'gopn on revoked -> TypeError');

// revoke DURING ownKeys trap
const { proxy: pb, revoke: rb } = Proxy.revocable({ x: 1 }, {
  ownKeys(t) { rb(); return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
});
ok(ename(() => Object.keys(pb)) === 'TypeError', 'revoke inside ownKeys -> TypeError');
__L(3, 'RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));

__FINISH("modules_ext");
