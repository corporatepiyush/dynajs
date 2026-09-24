// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["chaos-values=[\"A\",\"C\",4]"];
__EXP[2] = ["chaos-keys-after=[\"a\",\"c\",\"d\"]"];
__EXP[3] = ["chaos-values-again=[\"A\",\"C\",4]"];
__EXP[4] = ["DONE"];
// modules_ext c12: combined chaos on one object -- first getter deletes a future key,
// second getter re-adds it as NON-enumerable, third getter re-adds as enumerable.
// Printed sequence is the node-parity oracle (spec re-check semantics).
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o = { a: 1, b: 2, c: 3, d: 4 };
let stage = 0;
Object.defineProperty(o, 'a', {
  get() { stage = 1; delete o.b; return 'A'; }, enumerable: true, configurable: true,
});
Object.defineProperty(o, 'c', {
  get() {
    if (stage === 1) { o.b = 'B2'; Object.defineProperty(o, 'b', { enumerable: false, configurable: true }); stage = 2; }
    else if (stage === 2) { Object.defineProperty(o, 'b', { enumerable: true, configurable: true }); stage = 3; }
    return 'C';
  }, enumerable: true, configurable: true,
});
const v = Object.values(o);
__L(1, 'chaos-values=' + JSON.stringify(v));
__L(2, 'chaos-keys-after=' + JSON.stringify(Object.keys(o)));
__L(3, 'chaos-values-again=' + JSON.stringify(Object.values(o)));
__L(4, 'DONE');

summary("modules_ext");
