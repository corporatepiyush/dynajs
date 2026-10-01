__EXP = {};
__EXP[1] = ["add-future-values=[1,2,3]"];
__EXP[2] = ["keys-after=[\"a\",\"b\",\"c\",\"late\"]"];
__EXP[3] = ["add-future-entries=[[\"x\",1],[\"y\",2]]"];
__EXP[4] = ["add-then-delete=[1,2]"];
__EXP[5] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o = { a: 1, b: 2, c: 3 };
Object.defineProperty(o, 'a', {
  get() { o.late = 'L'; return 1; }, enumerable: true, configurable: true,
});
__L(1, 'add-future-values=' + JSON.stringify(Object.values(o)));
__L(2, 'keys-after=' + JSON.stringify(Object.keys(o)));
const o2 = { x: 1, y: 2 };
Object.defineProperty(o2, 'x', { get() { o2.z = 9; return 1; }, enumerable: true, configurable: true });
__L(3, 'add-future-entries=' + JSON.stringify(Object.entries(o2)));
const o3 = { p: 1, q: 2 };
Object.defineProperty(o3, 'p', { get() { o3.tmp = 5; return 1; }, enumerable: true, configurable: true });
Object.defineProperty(o3, 'q', { get() { delete o3.tmp; return 2; }, enumerable: true, configurable: true });
__L(4, 'add-then-delete=' + JSON.stringify(Object.values(o3)));
__L(5, 'DONE');

summary("modules_ext");
