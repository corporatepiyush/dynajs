// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["add-future-values=[1,2,3]"];
__EXP[2] = ["keys-after=[\"a\",\"b\",\"c\",\"late\"]"];
__EXP[3] = ["add-future-entries=[[\"x\",1],[\"y\",2]]"];
__EXP[4] = ["add-then-delete=[1,2]"];
__EXP[5] = ["DONE"];
// modules_ext c01: getter ADDS a future key during Object.values. Spec: ownKeys is a
// snapshot taken before the loop -> the added key must NOT appear. Node is oracle for
// the exact printed sequences.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o = { a: 1, b: 2, c: 3 };
Object.defineProperty(o, 'a', {
  get() { o.late = 'L'; return 1; }, enumerable: true, configurable: true,
});
__L(1, 'add-future-values=' + JSON.stringify(Object.values(o)));
__L(2, 'keys-after=' + JSON.stringify(Object.keys(o)));
// same via entries
const o2 = { x: 1, y: 2 };
Object.defineProperty(o2, 'x', { get() { o2.z = 9; return 1; }, enumerable: true, configurable: true });
__L(3, 'add-future-entries=' + JSON.stringify(Object.entries(o2)));
// adding a key via the getter that is deleted again by the NEXT getter
const o3 = { p: 1, q: 2 };
Object.defineProperty(o3, 'p', { get() { o3.tmp = 5; return 1; }, enumerable: true, configurable: true });
Object.defineProperty(o3, 'q', { get() { delete o3.tmp; return 2; }, enumerable: true, configurable: true });
__L(4, 'add-then-delete=' + JSON.stringify(Object.values(o3)));
__L(5, 'DONE');

summary("modules_ext");
