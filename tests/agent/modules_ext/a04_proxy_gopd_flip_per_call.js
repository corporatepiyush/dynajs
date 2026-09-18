// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["call1=[\"x\",\"y\"]"];
__EXP[2] = ["call2=[]"];
__EXP[3] = ["different=true"];
__EXP[4] = ["DONE"];
// modules_ext a04: gOPD trap that FLIPS its answer on every call. First key() call sees
// pass 1; a second key() call starts a fresh enumeration and must see the OTHER answer
// per key (snapshot is per-call, not cached). If dynajs cached anything, second call
// would equal the first. Node is the oracle for the observed sequences.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const p = new Proxy({ x: 1, y: 2 }, {
  ownKeys(t) { return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor(t, k) {
    const n = calls[k] = (calls[k] || 0) + 1;
    return { value: t[k], writable: true, enumerable: n % 2 === 1, configurable: true };
  },
});
const calls = {};
const r1 = Object.keys(p);
__L(1, 'call1=' + JSON.stringify(r1));
const r2 = Object.keys(p);
__L(2, 'call2=' + JSON.stringify(r2));
__L(3, 'different=' + (JSON.stringify(r1) !== JSON.stringify(r2)));
__L(4, 'DONE');

summary("modules_ext");
