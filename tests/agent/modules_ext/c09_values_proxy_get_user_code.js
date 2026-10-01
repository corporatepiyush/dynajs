__EXP = {};
__EXP[1] = ["proxy-del=[1,2]"];
__EXP[2] = ["proxy-del-keys=[\"a\",\"b\"]"];
__EXP[3] = ["proxy-add=[1,2]"];
__EXP[4] = ["DONE"];
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
const t2 = { a: 1, b: 2 };
const p2 = new Proxy(t2, {
  get(t, k) { if (k === 'a') t.n1 = 'N'; return t[k]; },
  getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
  ownKeys(t) { return Reflect.ownKeys(t); },
});
__L(3, 'proxy-add=' + JSON.stringify(Object.values(p2)));
__L(4, 'DONE');

summary("modules_ext");
