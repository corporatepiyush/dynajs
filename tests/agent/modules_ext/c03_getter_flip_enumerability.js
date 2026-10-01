__EXP = {};
__EXP[1] = ["flip-on=[1,2]"];
__EXP[2] = ["flip-off=[1]"];
__EXP[3] = ["flip-off-entries=[[\"a\",1]]"];
__EXP[4] = ["DONE"];
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o1 = { a: 1 };
Object.defineProperty(o1, 'sneaky', { value: 2, enumerable: false, configurable: true });
Object.defineProperty(o1, 'a', {
  get() { Object.defineProperty(o1, 'sneaky', { value: 2, enumerable: true, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(1, 'flip-on=' + JSON.stringify(Object.values(o1)));
const o2 = { a: 1, b: 2 };
Object.defineProperty(o2, 'a', {
  get() { Object.defineProperty(o2, 'b', { value: 2, enumerable: false, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(2, 'flip-off=' + JSON.stringify(Object.values(o2)));
const o3 = { a: 1, b: 2 };
Object.defineProperty(o3, 'a', {
  get() { Object.defineProperty(o3, 'b', { value: 2, enumerable: false, configurable: true }); return 1; },
  enumerable: true, configurable: true,
});
__L(3, 'flip-off-entries=' + JSON.stringify(Object.entries(o3)));
__L(4, 'DONE');

summary("modules_ext");
