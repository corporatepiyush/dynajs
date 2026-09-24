// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["values=[1,2]"];
__EXP[2] = ["seq=ownKeys,gopd:a,get:a,gopd:b,get:b"];
__EXP[3] = ["entries=[[\"a\",1],[\"b\",2]]"];
__EXP[4] = ["seq=ownKeys,gopd:a,get:a,gopd:b,get:b"];
__EXP[5] = ["DONE"];
// modules_ext a02: Object.values/entries(proxy) trap call sequence. VALUE/KEY_AND_VALUE
// kinds keep the per-key re-derive, so a gOPD trap must be observed per key (once during
// the ENUM_ONLY filter, matching V8; a SECOND gOPD per key would be a divergence).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const log = [];
function mk(obj) {
  return new Proxy(obj, {
    ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },
    getOwnPropertyDescriptor(t, k) { log.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
    get(t, k) { log.push('get:' + String(k)); return t[k]; },
  });
}
const target = { a: 1, b: 2 };
log.length = 0; const v = Object.values(mk(target));
__L(1, 'values=' + JSON.stringify(v));
__L(2, 'seq=' + log.join(','));
log.length = 0; const e = Object.entries(mk(target));
__L(3, 'entries=' + JSON.stringify(e));
__L(4, 'seq=' + log.join(','));
__L(5, 'DONE');

summary("modules_ext");
