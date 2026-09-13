// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["numkeys=[\"zero\",\"ONE\",\"two\"]"];
__EXP[2] = ["numkeys-keys=[\"0\",\"1\",\"2\"]"];
__EXP[3] = ["chain=[1,10,11]"];
__EXP[4] = ["chain-entries=[[\"a\",1],[\"b\",10],[\"c\",11]]"];
__EXP[5] = ["shared-identity=true"];
__EXP[6] = ["DONE"];
// modules_ext c08: key conversion during values: property whose KEY must be
// stringified (number keys) plus getters that read OTHER members. Verify values()
// output reflects live reads, not stale snapshot values, when two getters chain.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const o = { 2: 'two', 1: 'one', 0: 'zero' };
Object.defineProperty(o, '1', { get() { return 'ONE'; }, enumerable: true, configurable: true });
__L(1, 'numkeys=' + JSON.stringify(Object.values(o)));
__L(2, 'numkeys-keys=' + JSON.stringify(Object.keys(o)));
// chained getters: c reads b which reads a
const p = { a: 1, b: 2, c: 3 };
Object.defineProperty(p, 'b', { get() { return p.a * 10; }, enumerable: true, configurable: true });
Object.defineProperty(p, 'c', { get() { return p.b + 1; }, enumerable: true, configurable: true });
__L(3, 'chain=' + JSON.stringify(Object.values(p)));
__L(4, 'chain-entries=' + JSON.stringify(Object.entries(p)));
// getter returns objects (identity, not copied)
const shared = { n: 1 };
const q = { a: shared, b: shared };
const vq = Object.values(q);
__L(5, 'shared-identity=' + (vq[0] === shared && vq[1] === shared));
__L(6, 'DONE');

summary("modules_ext");
