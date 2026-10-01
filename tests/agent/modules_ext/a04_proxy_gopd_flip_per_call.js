__EXP = {};
__EXP[1] = ["call1=[\"x\",\"y\"]"];
__EXP[2] = ["call2=[]"];
__EXP[3] = ["different=true"];
__EXP[4] = ["DONE"];
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
