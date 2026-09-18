// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["readd-visited=[1,2]"];
__EXP[2] = ["freeze-mid=[1,2,3]"];
__EXP[3] = ["seal-mid=[1,2,3]"];
__EXP[4] = ["add-500: len=2 first=1 second=0"];
__EXP[5] = ["DONE"];
// modules_ext c07: getter mutates THIS in nastier ways during values(): adds a key
// BEFORE the current visit position (already-passed key re-added), converts the
// object to non-extensible mid-sweep, redefines length-like props. Node parity on
// all printed sequences.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
// re-add an ALREADY-VISITED key: must NOT be revisited
const o1 = { a: 1, b: 2 };
Object.defineProperty(o1, 'b', { get() { delete o1.a; o1.a = 100; return 2; }, enumerable: true, configurable: true });
__L(1, 'readd-visited=' + JSON.stringify(Object.values(o1)));
// freeze the object inside the first getter: rest of sweep continues (freeze only
// locks further mutation, existing enum props stay enumerable)
const o2 = { a: 1, b: 2, c: 3 };
Object.defineProperty(o2, 'a', { get() { Object.freeze(o2); return 1; }, enumerable: true, configurable: true });
__L(2, 'freeze-mid=' + JSON.stringify(Object.values(o2)));
// seal mid-sweep
const o3 = { a: 1, b: 2, c: 3 };
Object.defineProperty(o3, 'a', { get() { Object.seal(o3); return 1; }, enumerable: true, configurable: true });
__L(3, 'seal-mid=' + JSON.stringify(Object.values(o3)));
// getter adds 500 keys mid-sweep (stress the free path of the snapshot)
const o4 = { a: 1, z0: 0 };
Object.defineProperty(o4, 'a', { get() { for (let i = 0; i < 500; i++) o4['ad' + i] = i; return 1; }, enumerable: true, configurable: true });
const v4 = Object.values(o4);
__L(4, 'add-500: len=' + v4.length + ' first=' + v4[0] + ' second=' + v4[1]);
__L(5, 'DONE');

summary("modules_ext");
