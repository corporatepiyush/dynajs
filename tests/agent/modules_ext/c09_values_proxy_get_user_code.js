// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["proxy-del=[1,2]"];
__EXP[2] = ["proxy-del-keys=[\"a\",\"b\"]"];
__EXP[3] = ["proxy-add=[1,2]"];
__EXP[4] = ["DONE"];
// modules_ext c09: values/entries on a proxy where the GET trap is the user code that
// mutates the target -- future-key delete/add through the proxy target mid-sweep.
// Node parity on printed sequences.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const target = { a: 1, b: 2, c: 3 };
const p = new Proxy(target, {
  get(t, k) {
    if (k === 'a') { delete t.c; }
    return t[k];
  },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
  ownKeys(t) { return Reflect.ownKeys(t); },
});
__L(1, 'proxy-del=' + JSON.stringify(Object.values(p)));
__L(2, 'proxy-del-keys=' + JSON.stringify(Object.keys(target)));
// get trap ADDS future keys
const t2 = { a: 1, b: 2 };
const p2 = new Proxy(t2, {
  get(t, k) { if (k === 'a') t.n1 = 'N'; return t[k]; },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
  ownKeys(t) { return Reflect.ownKeys(t); },
});
__L(3, 'proxy-add=' + JSON.stringify(Object.values(p2)));
__L(4, 'DONE');

summary("modules_ext");
