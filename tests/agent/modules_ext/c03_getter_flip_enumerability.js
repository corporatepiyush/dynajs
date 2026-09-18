// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["flip-on=[1,2]"];
__EXP[2] = ["flip-off=[1]"];
__EXP[3] = ["flip-off-entries=[[\"a\",1]]"];
__EXP[4] = ["DONE"];
// modules_ext c03: getter FLIPS the enumerability of a future key via
// defineProperty during Object.values. Spec-literal: the full own-key list is
// re-checked per key, so a key made enumerable AFTER the snapshot must be INCLUDED
// (it was in the list), and a key made NON-enumerable must be dropped. Node may
// deviate -- the runner byte-compare is the verdict; both sequences are printed.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
// case 1: non-enum key becomes enum during the sweep -> included?
const o1 = { a: 1 };
Object.defineProperty(o1, 'sneaky', { value: 2, enumerable: false, configurable: true });
Object.defineProperty(o1, 'a', {
  get() { Object.defineProperty(o1, 'sneaky', { value: 2, enumerable: true, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(1, 'flip-on=' + JSON.stringify(Object.values(o1)));
// case 2: enum key becomes non-enum during the sweep -> dropped?
const o2 = { a: 1, b: 2 };
Object.defineProperty(o2, 'a', {
  get() { Object.defineProperty(o2, 'b', { value: 2, enumerable: false, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(2, 'flip-off=' + JSON.stringify(Object.values(o2)));
// case 3: same via entries
const o3 = { a: 1, b: 2 };
Object.defineProperty(o3, 'a', {
  get() { Object.defineProperty(o3, 'b', { value: 2, enumerable: false, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(3, 'flip-off-entries=' + JSON.stringify(Object.entries(o3)));
__L(4, 'DONE');

summary("modules_ext");
