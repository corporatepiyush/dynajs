// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["keys=[\"a\",\"b\",\"c\"]"];
__EXP[2] = ["seq=ownKeys,gopd:a,gopd:b,gopd:c"];
__EXP[3] = ["gopn=[\"a\",\"b\",\"c\"]"];
__EXP[4] = ["seq=ownKeys"];
__EXP[5] = ["gops=[]"];
__EXP[6] = ["seq=ownKeys"];
__EXP[7] = ["DONE"];
// modules_ext a01: Object.keys(proxy) trap call sequence -- GPN2 fast path (KEY kind
// trusts the GPN snapshot, so the per-key gOPD re-derive must be GONE: exactly one
// gOPD trap call per key, after the single ownKeys call). Node (V8) is the oracle.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
function mk(obj) {
  return new Proxy(obj, {
    ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },
    getOwnPropertyDescriptor(t, k) { log.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
    get(t, k) { log.push('get:' + String(k)); return t[k]; },
  });
}
const target = { a: 1, b: 2, c: 3 };
log.length = 0; const keys = Object.keys(mk(target));
__L(1, 'keys=' + JSON.stringify(keys));
__L(2, 'seq=' + log.join(','));
log.length = 0; const gopn = Object.getOwnPropertyNames(mk(target));
__L(3, 'gopn=' + JSON.stringify(gopn));
__L(4, 'seq=' + log.join(','));
log.length = 0; const gops = Object.getOwnPropertySymbols(mk(target));
__L(5, 'gops=' + JSON.stringify(gops));
__L(6, 'seq=' + log.join(','));
__L(7, 'DONE');

summary("modules_ext");
